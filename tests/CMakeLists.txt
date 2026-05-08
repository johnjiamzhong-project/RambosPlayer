find_package(Qt5 REQUIRED COMPONENTS Test Core)
find_package(FFMPEG REQUIRED)

function(add_qt_test name source)
    add_executable(${name} ${source})
    target_include_directories(${name} PRIVATE
        ${FFMPEG_INCLUDE_DIRS}
        ${CMAKE_SOURCE_DIR}/src
    )
    target_link_libraries(${name} PRIVATE
        Qt5::Test
        Qt5::Core
        ${FFMPEG_LIBRARIES}
    )
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# Tests enabled as each task is completed:

# Task 2 - FrameQueue
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tst_framequeue.cpp")
    add_qt_test(TstFrameQueue tst_framequeue.cpp)
endif()

# Task 3 - AVSync
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tst_avsync.cpp")
    add_qt_test(TstAVSync tst_avsync.cpp)
    target_sources(TstAVSync PRIVATE ${CMAKE_SOURCE_DIR}/src/avsync.cpp)
endif()

# Task 4 - DemuxThread
# if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tst_demuxthread.cpp")
#     add_qt_test(TstDemuxThread tst_demuxthread.cpp)
# endif()
