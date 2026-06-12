# 自定义 FFmpeg 查找模块（仅用于交叉编译场景，见 toolchain-aarch64-linux-gnu.cmake）。
#
# 目标 sysroot（FFMPEG_ROOT，默认 ~/sysroot/ffmpeg_rkmpp）下的 pkgconfig 文件
# prefix 指向板卡上的路径（/home/firefly/ffmpeg_rkmpp），在本机不存在，
# 因此不走 pkg-config，直接在 ${FFMPEG_ROOT}/include、${FFMPEG_ROOT}/lib 下
# find_path / find_library 各组件库，产出 FFMPEG_INCLUDE_DIRS / FFMPEG_LIBRARIES。

set(_ffmpeg_components
    avformat
    avcodec
    avutil
    avfilter
    avdevice
    swscale
    swresample
)

find_path(FFMPEG_INCLUDE_DIR
    NAMES libavcodec/avcodec.h
    PATHS "${FFMPEG_ROOT}/include"
    NO_DEFAULT_PATH
)

set(FFMPEG_LIBRARIES "")
foreach(_comp ${_ffmpeg_components})
    find_library(FFMPEG_${_comp}_LIBRARY
        NAMES ${_comp}
        PATHS "${FFMPEG_ROOT}/lib"
        NO_DEFAULT_PATH
    )
    mark_as_advanced(FFMPEG_${_comp}_LIBRARY)
    if(FFMPEG_${_comp}_LIBRARY)
        list(APPEND FFMPEG_LIBRARIES ${FFMPEG_${_comp}_LIBRARY})
    endif()
endforeach()

set(FFMPEG_INCLUDE_DIRS ${FFMPEG_INCLUDE_DIR})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(FFMPEG
    REQUIRED_VARS FFMPEG_INCLUDE_DIR FFMPEG_LIBRARIES
)

mark_as_advanced(FFMPEG_INCLUDE_DIR)
