cmake_minimum_required(VERSION 3.21)
project(emby-pc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Widgets 供骨架使用；Network 预留给 src/emby（Emby API 基于 QNetworkAccessManager）。
find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network)
qt_standard_project_setup()

set(LIBMPV_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libs/libmpv")

# ---------------------------------------------------------------------------
# 可选模块接线（T3 负责 src/emby，T4 负责 src/ui，与本骨架并行开发）
#
# 约定：模块目录下自带 CMakeLists.txt 并 define 静态库目标 emby / ui。
# 只有目录存在时才 add_subdirectory 并链接；目录不存在时骨架照常构建。
# 为兼容可能的命名，emby 回退 emby_core，ui 回退 emby_ui。
# ---------------------------------------------------------------------------
set(EMBY_PC_MODULE_LIBS "")
set(EMBY_PC_MODULE_NAMES "")

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/emby/CMakeLists.txt")
    add_subdirectory(src/emby)
    list(APPEND EMBY_PC_MODULE_NAMES "src/emby")
    if(TARGET emby)
        list(APPEND EMBY_PC_MODULE_LIBS emby)
    elseif(TARGET emby_core)
        list(APPEND EMBY_PC_MODULE_LIBS emby_core)
    else()
        message(WARNING "src/emby/CMakeLists.txt 存在，但未找到 emby / emby_core 目标，已跳过链接。")
    endif()
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/ui/CMakeLists.txt")
    add_subdirectory(src/ui)
    list(APPEND EMBY_PC_MODULE_NAMES "src/ui")
    if(TARGET ui)
        list(APPEND EMBY_PC_MODULE_LIBS ui)
    elseif(TARGET emby_ui)
        list(APPEND EMBY_PC_MODULE_LIBS emby_ui)
    else()
        message(WARNING "src/ui/CMakeLists.txt 存在，但未找到 ui / emby_ui 目标，已跳过链接。")
    endif()
endif()

set(APP_SOURCES
    src/main.cpp
    src/mainwindow.cpp src/mainwindow.h
    src/videohost.cpp src/videohost.h
    src/mpvplayer.cpp src/mpvplayer.h
)

# qt_add_executable 而非裸 add_executable：Windows 上会自动接 Qt6::EntryPoint
#（WIN32 GUI 程序的 WinMain 入口），MinGW/MSVC 都适用。
qt_add_executable(emby-pc WIN32 ${APP_SOURCES})
target_link_libraries(emby-pc PRIVATE Qt6::Widgets ${EMBY_PC_MODULE_LIBS})
target_include_directories(emby-pc PRIVATE "${LIBMPV_DIR}/include")

if(EMBY_PC_MODULE_LIBS)
    message(STATUS "emby-pc: 已接入可选模块：${EMBY_PC_MODULE_NAMES}（链接 ${EMBY_PC_MODULE_LIBS}）")
else()
    message(STATUS "emby-pc: 未发现 src/emby 或 src/ui，仅构建骨架")
endif()

if(WIN32)
    target_link_libraries(emby-pc PRIVATE user32)
    if(EXISTS "${LIBMPV_DIR}/libmpv-2.dll")
        add_custom_command(TARGET emby-pc POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${LIBMPV_DIR}/libmpv-2.dll" "$<TARGET_FILE_DIR:emby-pc>"
            COMMENT "Copying libmpv-2.dll")
    else()
        message(WARNING "libs/libmpv/libmpv-2.dll not found - put libmpv there before running.")
    endif()
endif()
