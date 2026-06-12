# ARM64 (aarch64-linux-gnu) 交叉编译工具链文件
#
# 目标：Rockchip aarch64 Linux 板卡。
# - Qt5 / 系统库：使用宿主机已安装的 arm64 multiarch 包
#   （/usr/lib/aarch64-linux-gnu、/usr/include/aarch64-linux-gnu，
#    通过 `dpkg --add-architecture arm64` + apt 安装的 :arm64 开发包）
# - FFmpeg（带 rkmpp 硬解）：来自板卡 rootfs 拷贝 ~/sysroot/ffmpeg_rkmpp，
#   该目录下 pkgconfig 文件的 prefix 路径在本机不存在，故由 cmake/FindFFMPEG.cmake
#   通过 FFMPEG_ROOT 直接 find_path/find_library 定位，不依赖 pkg-config。

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 板卡 rootfs 拷贝，FFmpeg(rkmpp) 即位于其下的 ffmpeg_rkmpp 目录
if(NOT DEFINED SYSROOT_DIR)
    set(SYSROOT_DIR "$ENV{HOME}/sysroot" CACHE PATH "Target board rootfs copy")
endif()

if(NOT DEFINED FFMPEG_ROOT)
    set(FFMPEG_ROOT "${SYSROOT_DIR}/ffmpeg_rkmpp" CACHE PATH "FFmpeg (rkmpp) install root")
endif()
