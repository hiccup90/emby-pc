cmake_minimum_required(VERSION 3.21)

# 两种用法：
# 1) 被 app/CMakeLists.txt add_subdirectory(src/ui) 引入：只构建静态库 ui；
# 2) 单独构建本目录（调试/截图）：额外构建 emby-ui-demo 可执行文件。
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(emby-pc-ui LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(EMBY_UI_STANDALONE ON)
else()
    set(EMBY_UI_STANDALONE OFF)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network)

set(CMAKE_AUTOMOC ON)

# UI 的登录/会话功能依赖 app/src/emby 的 EmbyClient。
# 被 app/CMakeLists.txt 引入时 emby 目标通常已存在；单独构建本目录时自动补上。
if(NOT TARGET emby)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../emby/CMakeLists.txt")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../emby" "${CMAKE_CURRENT_BINARY_DIR}/emby")
    else()
        message(FATAL_ERROR "app/src/ui 需要 app/src/emby（EmbyClient）模块")
    endif()
endif()

set(UI_SOURCES
    uitypes.h
    authutils.cpp authutils.h
    tokenstore.cpp tokenstore.h
    logindialog.cpp logindialog.h
    playbackerrordialog.cpp playbackerrordialog.h
    theme.cpp theme.h
    icons.cpp icons.h
    postergenerator.cpp postergenerator.h
    thumbnailcache.cpp thumbnailcache.h
    embythumbnailcache.cpp embythumbnailcache.h
    mocklibrary.cpp mocklibrary.h
    librarydata.cpp librarydata.h
    mediamodel.cpp mediamodel.h
    posterdelegate.cpp posterdelegate.h
    libraryview.cpp libraryview.h
    homeview.cpp homeview.h
    detailview.cpp detailview.h
    playerpage.cpp playerpage.h
    sidebar.cpp sidebar.h
    settingspage.cpp settingspage.h
    titlebar.cpp titlebar.h
    mainwindow.cpp mainwindow.h
)

add_library(ui STATIC ${UI_SOURCES})
add_library(emby::ui ALIAS ui)

target_compile_features(ui PUBLIC cxx_std_20)
target_link_libraries(ui PUBLIC Qt6::Widgets)
if(TARGET emby)
    target_link_libraries(ui PUBLIC emby)
endif()
target_include_directories(ui
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(ui PRIVATE EMBY_UI_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

qt_add_resources(ui "ui_style"
    PREFIX "/ui"
    BASE "style"
    FILES style/dark.qss
)

option(EMBY_UI_BUILD_DEMO "Build the standalone UI demo / screenshot tool" ${EMBY_UI_STANDALONE})
if(EMBY_UI_BUILD_DEMO)
    # 独立 demo 需要 WindowFrame 符号（T26），app 目标则由 app/CMakeLists.txt 提供。
    add_executable(emby-ui-demo demo_main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../windowframe.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/../windowframe.h)
    target_link_libraries(emby-ui-demo PRIVATE ui)
    target_include_directories(emby-ui-demo PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()
