# 构建验证报告（T1：app/ 骨架 Linux 编译）

> 任务：task-4 · 负责人：build-verifier · 日期：2026-11-04
> 范围：`app/`（根 CMakeLists、src 现有文件、scripts/）、本报告。
> 不包含 `app/src/emby`、`app/src/ui` 的源码（由 T3/T4 并行实现，本任务只做条件接线）。

## 1. 结论

- **通过**：Qt6 Widgets + libmpv 骨架（方案 A：`--wid` + mpv OSC）在 Linux 容器中 **configure / build / link 全部成功**，产物为 `app/build-linux/emby-pc`。
- 修复 **1 个编译错误**（`mpvplayer.cpp` 的 `QVector<const char*>` 传参）和 **1 个潜在生命周期 bug**（`setOption` 收到临时 QByteArray 指针）。
- 完成 **模块化 CMake 条件接线**：`src/emby/CMakeLists.txt`、`src/ui/CMakeLists.txt` 存在时自动 `add_subdirectory` 并链接；不存在时骨架照常构建。已用 mock 模块验证主/回退目标名两条分支，并用 T3/T4 的真实模块做了全量集成构建（`emby` + `ui`，通过）。
- 新增 `app/scripts/build-linux.sh`，README 增加 Linux 编译验证小节。
- **范围限制**：Linux 只能验证编译/链接；Windows 的 D3D11、`--wid` 嵌入、HDR/DV tone-map、mpv OSC 运行效果均无法在本容器验证（详见第 7 节）。

## 2. 环境

| 项 | 值 |
|---|---|
| OS | Debian GNU/Linux 13 (trixie)，x86_64，容器（无 Windows、无 GPU） |
| CMake | 3.31.6 |
| 编译器 | g++ 14.2.0 (Debian 14.2.0-19) |
| Qt | Qt 6.8.2，`qt6-base-dev=6.8.2+dfsg-9+deb13u2`（Widgets、Network 可用） |
| libmpv 头文件 | `app/libs/libmpv/include/mpv/`，`MPV_CLIENT_API_VERSION = 2.6`（仅头文件） |
| libmpv 运行库 | **无**（Windows 构建需按 `app/libs/libmpv/README.md` 放入 `libmpv-2.dll`） |

## 3. 构建命令与结果

### 3.1 新增脚本（推荐）

```bash
cd /root/github/emby-pc/app
./scripts/build-linux.sh
# 或指定目录/类型：
BUILD_DIR=/tmp/emby-pc-release BUILD_TYPE=Release ./scripts/build-linux.sh
```

脚本等价于：

```bash
cmake -S app -B app/build-linux -DCMAKE_BUILD_TYPE=Debug
cmake --build app/build-linux --parallel "$(nproc)"
```

### 3.2 验证结果

- **Debug 干净构建**（先删除 `app/build`、`app/build-linux`）：`exit 0`，全部 5 个编译单元 + moc 编译通过，`Linking CXX executable emby-pc`，`[100%] Built target emby-pc`。
- **Release 构建（仅骨架，初始阶段）**：`exit 0`，产物 `/tmp/emby-pc-release/emby-pc`（107 KB，Debug 版 2.3 MB）。
- **全量 Release 干净构建（含真实 `src/emby` + `src/ui` 模块）**：`BUILD_DIR=/tmp/emby-pc-release-modules BUILD_TYPE=Release ./scripts/build-linux.sh`，`exit 0`；configure 提示 `已接入可选模块：src/emby;src/ui（链接 emby;ui）`，`libemby.a`、`libui.a`、`emby-pc` 全部构建成功。
- **产物检查**：ELF 64-bit x86-64 PIE；`ldd` 显示正常链接 `libQt6Widgets.so.6`、`libQt6Gui.so.6`、`libQt6Core.so.6`、`libQt6DBus.so.6`，无 missing。
- **告警检查**：额外用 `-DCMAKE_CXX_FLAGS="-Wall -Wextra"` 构建，**无编译告警**。
- Configure 时提示 `emby-pc: 未发现 src/emby 或 src/ui，仅构建骨架`（当时两个模块目录尚未出现 CMakeLists.txt），符合预期。

## 4. 修复的 bug

| # | 文件/位置 | 问题 | 修复 |
|---|---|---|---|
| 1 | `app/src/mpvplayer.cpp` 的 `MpvPlayer::command()`（原第 229 行） | Qt6 中 `QVector` 是 `QList` 的别名，`QList<const char*>::constData()` 返回 `const char* const*`，不能转换为 `mpv_command()` 需要的 `const char**`。这是**实际编译错误**：`invalid conversion from 'const char* const*' to 'const char**' [-fpermissive]`。 | 改用 `std::vector<QByteArray> storage` + `std::vector<const char*> argv`，用 `argv.data()` 传入；`storage` 先全部生成，保证 `constData()` 指针在调用期间有效。 |
| 2 | `app/src/mpvplayer.cpp` 的 `MpvPlayer::initialize()`（原第 135 行） | `setOption(QStringLiteral("wid").toUtf8().constData(), ...)` 把临时 `QByteArray` 的内部指针当 `const char *name` 传入。当前调用在完整表达式内尚安全，但属于悬空指针隐患（一旦 `setOption` 内部保存指针或后续重构就会出错）。 | 改为稳定的字符串字面量：`setOption("wid", QString::number(windowId))`。 |
| 3 | `app/CMakeLists.txt` | `add_executable(emby-pc WIN32 ...)` 在 Windows/MSVC 下是 GUI 子系统和 `WinMain` 入口，裸 `add_executable` 不会自动链接 Qt6 的 `Qt6::EntryPoint`，Windows 链接可能因缺少 `WinMain` 失败。 | 改用 Qt6 官方推荐的 `qt_add_executable(emby-pc WIN32 ...)`（自动处理 EntryPoint 和平台差异；Linux 行为不变）。 |
| 4 | `app/src/mpvplayer.cpp` 头文件区 | 删除了不再使用的 `<QVariant>`、`<QVector>`，新增 `<vector>`。 | 同上，保持 include 干净。 |

## 5. CMake 模块化接线

根 `app/CMakeLists.txt` 已按以下规则改造：

- **`src/emby/CMakeLists.txt` 存在** → `add_subdirectory(src/emby)`，并链接目标 `emby`（若不存在则尝试 `emby_core`）。
- **`src/ui/CMakeLists.txt` 存在** → `add_subdirectory(src/ui)`，并链接目标 `ui`（若不存在则尝试 `emby_ui`）。
- 两个目录都不存在 → 只构建骨架（初始骨架状态）。
- CMakeLists 存在但预期目标名不存在 → `message(WARNING ...)` 并跳过链接，configure 不失败。
- 根项目现在 `find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Network)`，为 `src/emby` 使用 `Qt6::Network` 预留依赖（`qt6-base-dev` 已包含）。
- 模块接入情况会在 configure 阶段打印 `emby-pc: 已接入可选模块：...`。

**验证：**

| 场景 | 结果 |
|---|---|
| 无 `src/emby`、`src/ui`（初始骨架状态） | configure 正常，提示“未发现…仅构建骨架”，build OK |
| mock 目标 `emby` + `ui`（`/tmp` 副本） | 提示 `已接入可选模块：src/emby;src/ui（链接 emby;ui）`，build OK |
| mock 回退目标 `emby_core` + `emby_ui`（`/tmp` 副本） | 提示链接 `emby_core;emby_ui`，build OK |
| mock 模块目录存在但目标名不匹配 | 打印 CMake Warning，configure 仍成功 |
| **真实 T3/T4 模块 `src/emby` + `src/ui`** | 提示 `已接入可选模块：src/emby;src/ui（链接 emby;ui）`；`libemby.a`、`libui.a`、`emby-pc` 全部构建成功，exit 0 |

> mock 分支测试在 `/tmp` 的 app 副本中进行，没有改动 T3/T4 的真实目录。真实模块第一次接入时 UI 模块曾有 2 处自有编译错误（`HeroBanner` 前向声明/定义不一致、`EpisodeRow` 缺 `Q_OBJECT`），已由 ui-builder 修复后复测通过；这些属于模块内部问题，不是 CMake 接线问题。
>
> 注意：当前 `app/src/main.cpp` 仍使用骨架自带的 `MainWindow`，`ui`/`emby` 静态库只是被链接、尚无符号被主程序引用（链接器可能因此丢弃其对象）；这不影响本任务的“接线+编译验证”结论，但模块功能本身尚未接入主程序。

## 6. 新增/修改文件清单

| 文件 | 变更 |
|---|---|
| `app/CMakeLists.txt` | `qt_add_executable` + Qt6 Network + 可选模块条件 add_subdirectory/链接 |
| `app/src/mpvplayer.cpp` | 修复命令参数数组编译错误；`wid` 选项改用稳定字符串；清理 include |
| `app/scripts/build-linux.sh` | 新增：cmake configure + build（支持 `BUILD_DIR`、`BUILD_TYPE` 环境变量） |
| `app/README.md` | 增加“Linux（仅编译验证）”小节，Windows 构建说明保留 |
| `reports/build-report.md` | 本报告 |

## 7. 已知风险 / Linux 无法验证项

1. **Windows 实机构建未验证**：MSVC 2022 / MinGW 的 `qt_add_executable` + `Qt6::EntryPoint`、`user32` 链接、`libmpv-2.dll` POST_BUILD 拷贝均只在 Linux 上做了 CMake 逻辑验证，需在 Windows 机器上跑一次 `cmake --build`。
2. **`--wid` 子窗口嵌入**：`VideoHost::nativeWindow()` 返回 HWND 的路径、mpv 子窗口尺寸同步（`videohost.cpp` 的 `GetWindow/MoveWindow`）均只能在 Windows 验证。
3. **HDR/DV + 硬解**：`gpu-api=d3d11`、`gpu-context=d3d11`、`hwdec=d3d11va`、`target-trc=srgb`/`target-prim=bt.709`/`tone-mapping=auto` 的实际效果（不绿、不发灰）以及 D3D11VA 是否真正零拷贝，Linux 无 GPU/D3D11，无法验证。
4. **运行时验证缺失**：Linux 产物启动会因找不到 `libmpv-2.dll` 弹初始化失败对话框（`loadApi` 只按 Windows 文件名查找），属预期；容器无 X11/显示，未做 GUI 运行测试。**编译验证通过 ≠ 播放功能通过。**
5. **Qt 版本**：本机用 Qt 6.8.2 验证；CMake 要求 Qt ≥ 6.5。代码只用了 Widgets 基础 API，理论兼容 6.5，但未在 6.5 上实测。
6. **libmpv 版本/ABI**：头文件 `MPV_CLIENT_API_VERSION 2.6`，实际放入的 `libmpv-2.dll` 版本需匹配（只需稳定 client API 符号；`mpv_error_string` 已做可空保护）。建议在 Windows 上把 `mpv --version` 与 dll 版本记录到测试报告。
7. **真实模块后续变更风险**：T3/T4 的模块代码仍在并行迭代，本报告记录的是 2026-11-04 06:04 UTC 左右的全量集成构建结果；若它们之后改代码/CMake，需要重新执行 `./scripts/build-linux.sh`。当前真实目标名已确认是 `emby`/`ui`，与根 CMake 约定一致。

## 8. 后续建议（Windows 验收时）

1. Windows 上按 README 的 Visual Studio 命令构建，确认 `build/Release/emby-pc.exe` 生成且 `libmpv-2.dll` 被复制到同目录。
2. 用 HDR10、DV P5、DV P8、ASS 特效字幕四个样本验证：不绿屏、不发灰、字幕样式正确、`i` 统计中 `hwdec-current=d3d11va`。
3. 检查子窗口随主窗口 resize 是否无黑边/缝隙；全屏切换（`f`/Esc script-message）是否正常。
4. 若接入 T3/T4 模块后构建失败，先看 configure 打印的“已接入可选模块/警告”，再核对模块 CMakeLists 的目标名与 include 目录。
