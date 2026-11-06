cmake_minimum_required(VERSION 3.21)
project(emby-pc VERSION 0.1.1 LANGUAGES CXX)

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
    src/windowframe.cpp src/windowframe.h
    src/crashhandler.cpp src/crashhandler.h
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
    # user32: VideoHost/WindowFrame 的窗口样式与命中测试；dwmapi: DWM 扩展框；
    # shell32: SHAppBarMessage（任务栏自动隐藏处理）；dbghelp: MiniDumpWriteDump；
    # d3d11/dxgi: spike/render-d3d11 —— render API 的 D3D11 后端需要
    # ID3D11Device/ID3D11Texture2D 与 DXGI 交换链（CreateSwapChainForHwnd）。
    target_link_libraries(emby-pc PRIVATE user32 dwmapi shell32 dbghelp d3d11 dxgi)

    # -----------------------------------------------------------------------
    # spike/render-d3d11 临时依赖，Qt 版本锁定 6.8.x（CI 见 windows-build.yml）。
    # D3D11 纹理导入（D3D11 纹理 → QRhiTexture，QRhiD3D11NativeHandles 等）
    # 需要 Qt Gui 的私有头，只有 Qt6::GuiPrivate 提供；私有头不保证跨小版本
    # 兼容，本地请对齐 6.8.*，不要混用 6.5/6.9。
    # 找不到时只告警、不中断：骨架与业务代码不依赖它，仅 spike 导入路径需要。
    # -----------------------------------------------------------------------
    find_package(Qt6 QUIET COMPONENTS GuiPrivate)
    if(TARGET Qt6::GuiPrivate)
        target_link_libraries(emby-pc PRIVATE Qt6::GuiPrivate)
    else()
        message(WARNING "Qt6::GuiPrivate 不可用：D3D11 纹理导入（QRhi 私有头）无法编译，请安装 Qt 6.8.x 官方套件。")
    endif()

    # fetch-libmpv.ps1 会重建 include/mpv（仅保留仓库自带的 render_d3d11.h）：缺这个头时
    # 在配置阶段就提示，免得等到编译 mpvplayer/videohost 的 D3D11 分支才报错。
    if(NOT EXISTS "${LIBMPV_DIR}/include/mpv/render_d3d11.h")
        message(WARNING "libs/libmpv/include/mpv/render_d3d11.h 缺失：render API D3D11 无法编译（从 git 恢复该头文件）。")
    endif()
    if(EXISTS "${LIBMPV_DIR}/libmpv-2.dll")
        add_custom_command(TARGET emby-pc POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${LIBMPV_DIR}/libmpv-2.dll" "$<TARGET_FILE_DIR:emby-pc>"
            COMMENT "Copying libmpv-2.dll")
    else()
        message(WARNING "libs/libmpv/libmpv-2.dll not found - put libmpv there before running.")
    endif()
endif()

# ---------------------------------------------------------------------------
# T33/M4-1：Windows VERSIONINFO + 应用图标。
# version.rc 是模板：configure_file 用 project(VERSION) 与图标绝对路径生成到构建
# 目录，仅 WIN32 编译；Linux 构建不受影响。
# ---------------------------------------------------------------------------
if(WIN32)
    enable_language(RC)
    set(APP_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/resources/win/app.ico")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/win/version.rc"
        "${CMAKE_CURRENT_BINARY_DIR}/win/version.rc"
        @ONLY)
    target_sources(emby-pc PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/win/version.rc")
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/win/version.rc" PROPERTIES LANGUAGE RC)
endif()
