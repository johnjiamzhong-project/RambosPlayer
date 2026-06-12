cmake_minimum_required(VERSION 3.16)
project(RambosPlayer VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8 /FS)
endif()

# 交叉编译（ARM64）时使用项目自带的 FindFFMPEG.cmake，
# 因为目标 sysroot 下 FFmpeg 的 pkgconfig prefix 路径在本机不存在
if(CMAKE_CROSSCOMPILING)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets Multimedia Svg Network)
find_package(FFMPEG REQUIRED)

add_executable(RambosPlayer WIN32
    src/resources.qrc
    src/main.cpp
    src/logger.cpp
    src/avsync.cpp
    src/demuxthread.cpp
    src/hwaccel.cpp
    src/filtergraph.cpp
    src/videodecodethread.cpp
    src/audiodecodethread.cpp
    src/videorenderer.cpp
    src/playercontroller.cpp
    src/mainwindow.cpp
    src/filterpanel.cpp
    src/encodethread.cpp
    src/audioencodethread.cpp
    src/muxthread.cpp
    src/streamcontroller.cpp
    src/streamconfigdialog.cpp
    src/localrecorder.cpp
    src/thumbnailextractor.cpp
    src/browseclipper.cpp
    src/segmentclipper.cpp
    src/timeline.cpp
    src/exportworker.cpp
    src/httpflvserver.cpp
    src/streamdecoder.cpp
    src/streampipeline.cpp
    src/mpegtsserver.cpp
    src/concatdemuxer.cpp
    src/concatfilter.cpp
    src/mergeworker.cpp
    src/mergepanel.cpp
    src/audiomixworker.cpp
    src/audiomixpanel.cpp
    src/audiopreviewwindow.cpp
)


target_include_directories(RambosPlayer PRIVATE
    ${FFMPEG_INCLUDE_DIRS}
    src/
)

target_link_libraries(RambosPlayer PRIVATE
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::Multimedia
    Qt5::Svg
    Qt5::Network
    ${FFMPEG_LIBRARIES}
)

if(WIN32)
    target_link_libraries(RambosPlayer PRIVATE dbghelp)
endif()

if(CMAKE_CROSSCOMPILING)
    # FFmpeg(rkmpp) 依赖 librockchip_mpp（来自板卡 sysroot）；
    # libva* 等 VAAPI 库本机没有，运行时由板卡 rootfs 提供，链接期忽略其未解析符号
    target_link_directories(RambosPlayer PRIVATE "${SYSROOT_DIR}/usr/lib/aarch64-linux-gnu")
    target_link_options(RambosPlayer PRIVATE
        "-Wl,--allow-shlib-undefined"
        "-Wl,--unresolved-symbols=ignore-in-shared-libs"
    )
endif()

enable_testing()
add_subdirectory(tests EXCLUDE_FROM_ALL)
