cmake_minimum_required(VERSION 3.16)
project(RambosPlayer VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/utf-8)
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets Multimedia)
find_package(FFMPEG REQUIRED)

add_executable(RambosPlayer WIN32
    src/main.cpp
    src/logger.cpp
    src/avsync.cpp
    src/demuxthread.cpp
    src/videodecodethread.cpp
    src/audiodecodethread.cpp
    src/videorenderer.cpp
    src/playercontroller.cpp
    src/mainwindow.cpp
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
    ${FFMPEG_LIBRARIES}
)

if(WIN32)
    target_link_libraries(RambosPlayer PRIVATE dbghelp)
endif()

enable_testing()
add_subdirectory(tests EXCLUDE_FROM_ALL)
