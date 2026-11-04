# T5 集成与全量编译报告

> 任务：task-8 · 负责人：build-verifier · 日期：2026-11-04
> 范围：把 T1 骨架、T3 Emby API 模块、T4 UI 模块合成 app 可执行文件；不改 `app/src/emby`、`app/src/ui` 内部实现。

## 1. 结论

- **全量编译通过**：`app/` 根工程已用真实 `src/emby` + `src/ui` 模块构建成功（Debug，exit 0），产物 `app/build-linux/emby-pc`。
- **应用入口已切换**：`app/src/main.cpp` 使用 `ui::MainWindow` 作为应用壳；旧临时 `MainWindow` 已删除，`app/src/mainwindow.*` 改为 `PlayerController`（VideoHost + MpvPlayer 播放控制层）。
- **播放层已接到 PlayerPage**：首次打开本地文件时 `attachVideoSurface(VideoHost*)`，并把 PlayerPage 的播放/暂停、上一集/下一集、seek、音量、统计信号接到 MpvPlayer；mpv 的位置/时长/暂停/标题事件回填 PlayerPage。
- **本地文件/文件夹播放保留**：文件菜单提供打开文件（Ctrl+O）、打开文件夹（Ctrl+Shift+O），文件夹内视频自动组成 mpv 播放列表。
- **运行验证**：UI standalone selftest **24/24 PASS**；Emby 假服务器测试 **94 项 ok，0 failed，ALL TESTS PASSED**；集成 app 在 offscreen 平台启动 4 秒保持运行无崩溃。

## 2. 环境

| 项 | 值 |
|---|---|
| OS | Debian GNU/Linux 13 (trixie)，x86_64，容器（无 Windows/GPU/显示） |
| CMake | 3.31.6 |
| 编译器 | g++ 14.2.0 |
| Qt | 6.8.2（qt6-base-dev，Widgets + Network） |
| libmpv | 仅头文件 `app/libs/libmpv/include/mpv/`；容器内无 `libmpv-2.dll`（Windows 运行需放入） |
| UI 模块 | `app/src/ui`（T4，task-7 已完成） |
| Emby 模块 | `app/src/emby`（T3，task-6 已完成） |

## 3. 构建命令与结果

### 3.1 全量 Debug 构建（app 根工程）

```bash
cd /root/github/emby-pc/app
./scripts/build-linux.sh
# 等价于：
# cmake -S . -B build-linux -DCMAKE_BUILD_TYPE=Debug
# cmake --build build-linux -j"$(nproc)"
```

结果：

```text
-- emby-pc: 已接入可选模块：src/emby;src/ui（链接 emby;ui）
[ 25%] Built target emby
[ 78%] Built target ui
[100%] Linking CXX executable emby-pc
[100%] Built target emby-pc
```

- `libemby.a`、`libui.a` 均构建成功；`emby-pc` 链接成功，exit 0。
- Release 干净构建同样通过：`BUILD_DIR=/tmp/emby-pc-t5-release BUILD_TYPE=Release ./scripts/build-linux.sh`，exit 0。
- 额外用 `-DCMAKE_CXX_FLAGS="-Wall -Wextra"` 构建：无编译告警。
- `app/src/ui/demo_main.cpp` 只在 standalone demo 构建（`EMBY_UI_BUILD_DEMO=ON`）时编译，app 正式 target 没有包含它，因此与 app 的 `main.cpp` 不冲突。

### 3.2 UI selftest

```bash
cmake -S app/src/ui -B /tmp/emby-ui-t5 -DCMAKE_BUILD_TYPE=Debug -DEMBY_UI_BUILD_DEMO=ON
cmake --build /tmp/emby-ui-t5 -j"$(nproc)"
QT_QPA_PLATFORM=offscreen /tmp/emby-ui-t5/emby-ui-demo --selftest
```

结果：**24 项 PASS，ALL PASS**（mock 200 部、搜索过滤、滚动绘制、页面栈、播放页控制信号、attachVideoSurface/releaseVideoSurface、最小尺寸 960x600 等）。滚动基线 5 帧平均 18 ms/帧（offscreen 软件渲染）。

### 3.3 Emby 假服务器测试

```bash
cmake -S app/src/emby -B /tmp/emby-tests-t5 -DCMAKE_BUILD_TYPE=Debug -DEMBY_BUILD_TESTS=ON
cmake --build /tmp/emby-tests-t5 -j"$(nproc)"
ctest --test-dir /tmp/emby-tests-t5 --output-on-failure
```

结果：`1/1 Test #1: emby_tests ... Passed`；直接运行 `emby_tests` 共 **94 项 ok、0 FAIL、ALL TESTS PASSED**（认证、Views/Items/Resume、PlaybackInfo、直连 URL、进度上报、错误处理）。

### 3.4 集成 app offscreen 冒烟

```bash
QT_QPA_PLATFORM=offscreen timeout 4 ./app/build-linux/emby-pc
# exit=124：进程在事件循环中稳定运行，被 timeout 主动终止，无崩溃/无报错
```

## 4. 集成改动

| 文件 | 改动 |
|---|---|
| `app/src/main.cpp` | 改为 `ui::MainWindow` 应用壳；`findChild<ui::PlayerPage*>()` 取得播放页；创建 `PlayerController` 并安装菜单 |
| `app/src/mainwindow.h` | 旧 `MainWindow` 删除，改为 `PlayerController` 声明（持有 VideoHost/MpvPlayer，公开 openFile/openFolder/installMenus） |
| `app/src/mainwindow.cpp` | 实现 PlayerController：懒初始化 mpv、attachVideoSurface、控制信号→MpvPlayer、MpvPlayer→PlayerPage 回填、文件/文件夹菜单 |
| `app/README.md` | 同步运行说明与已知待办（UI 壳已接入） |
| `app/CMakeLists.txt` | 检查后沿用 T1 的条件 add_subdirectory(src/emby)、add_subdirectory(src/ui) 接线，无需再改 |
| `reports/integration-report.md` | 本报告 |
| `mockups/t5-preview/*.png` | offscreen UI 截图（见第 6 节） |

## 5. 播放层接线细节

### 5.1 启动与本地播放

1. `main.cpp` 创建 `ui::MainWindow`，默认进入首页（mock 媒体库）。
2. `PlayerController` 构造时创建隐藏的 `VideoHost` 和 `MpvPlayer`（此时不加载 libmpv）。
3. 用户通过 `文件 → 打开文件…` / `打开文件夹…` 选择本地视频后：
   - `ensurePlayer()` 首次初始化 MpvPlayer（`--wid` 使用 `VideoHost::nativeWindow()`）；
   - `PlayerPage::attachVideoSurface(m_videoHost)` 替换占位并显示真实视频区；
   - 调用 `ui::MainWindow::openPlayerDemo()` 切到播放页（借 mock 条目进入页面，随后用真实文件名覆盖标题）；
   - `MpvPlayer::loadPlaylist(files, startIndex)` 加载本地文件/文件夹播放列表；
   - 隐藏 `PlayerControlBar`（真实播放由 mpv OSC 提供控制条，Qt 控制条只作为 attach 前占位/调试）。

### 5.2 PlayerPage 控制信号 → MpvPlayer

| PlayerPage 信号 | 动作 |
|---|---|
| `playPauseRequested` | `MpvPlayer::togglePause()` |
| `previousEpisodeRequested` / `nextEpisodeRequested` | `MpvPlayer::playlistPrevious()` / `playlistNext()` |
| `seekRequested(qint64 ms)` | `MpvPlayer::seekAbsolute(ms/1000)` |
| `volumeChanged(int)` | `MpvPlayer::setVolume()` |
| `statsRequested` | `MpvPlayer::toggleStats()` |
| `subtitleTrackSelected` / `audioTrackSelected` | 当前仅状态栏提示（PlayerPage 的轨道列表是 mock，尚未映射 mpv sid/aid） |
| `backRequested` | 恢复 `PlayerControlBar`（返回详情页） |

### 5.3 MpvPlayer → PlayerPage 回填

- `positionChanged` / `durationChanged`：`PlayerPage::setPosition(posMs, durationMs)`，驱动进度条与时间标签。
- `pausedChanged`：`PlayerPage::setPlaying(!paused)`，同步播放/暂停图标。
- `titleChanged`：`PlayerPage::setEpisodeInfo(mediaTitle, 本地播放信息)`。
- `fileLoaded` / `endReached`：更新播放状态。
- mpv 的 `f` / `Esc` script-message：`MpvPlayer::toggleFullscreenRequested` / `exitFullscreenRequested` → 窗口全屏/恢复，并同步 `PlayerPage::setFullscreen`。

## 6. 运行截图（offscreen）

截图来自 `emby-ui-demo --screenshot`（UI 模块，与 app 使用同一 `ui::MainWindow`）：

| 页面 | 截图 |
|---|---|
| 首页 | [01-home.png](../mockups/t5-preview/01-home.png) |
| 电影库 | [02-movies.png](../mockups/t5-preview/02-movies.png) |
| 详情页 | [03-detail.png](../mockups/t5-preview/03-detail.png) |
| 详情页 · 剧集 | [04-detail-episodes.png](../mockups/t5-preview/04-detail-episodes.png) |
| 播放页（VideoHost 占位 + Qt 控制条） | [05-player.png](../mockups/t5-preview/05-player.png) |
| 最小尺寸 960x600 | [06-home-960x600.png](../mockups/t5-preview/06-home-960x600.png) |
| 设置页 | [07-settings.png](../mockups/t5-preview/07-settings.png) |
| 1080p 首页 | [08-home-1920x1080.png](../mockups/t5-preview/08-home-1920x1080.png) |
| 最小尺寸详情 | [09-detail-960x600.png](../mockups/t5-preview/09-detail-960x600.png) |
| 最小尺寸播放页 | [10-player-960x600.png](../mockups/t5-preview/10-player-960x600.png) |

## 7. 已知问题 / 未验证项

1. **真实视频播放未验证**：容器没有 `libmpv-2.dll`、GPU 和显示，`openFile()` 初始化 mpv 会失败并弹错误对话框；Windows 上需放入 `libmpv-2.dll` 后实测。
2. **Windows `--wid` 嵌入、D3D11VA、HDR/DV tone-map、mpv OSC 仍只停留在代码路径**，与 T1 报告相同的限制。
3. **UI 的 mock 播放逻辑与真实播放层共存**：PlayerPage 自带信号与 mock 状态仍由 `ui::MainWindow` 内部连接；真实播放时靠 mpv 事件回填覆盖状态。后续可把 PlayerPage 的 mock 逻辑拆成 demo 专用，避免双重状态。
4. **字幕/音轨选择未接真实 mpv**：PlayerPage 当前轨道列表是 mock（`sub-chi` 等），需要后续从 mpv 读取 track-list 并映射 sid/aid。
5. **文件菜单用 QMenuBar 附加在 ui::MainWindow 上**；深色 QSS 未专门定制 QMenuBar（QMenu 有样式），Windows 上可再调。
6. **全屏由 mpv script-message 触发时只调用窗口 showFullScreen/showNormal**，未同步 `ui::MainWindow` 内部的侧边栏/顶栏隐藏状态；通过 UI 全屏按钮进入时仍走 UI 原逻辑（正常）。后续可在 WindowController 统一。
7. **`app/src/ui/demo_main.cpp` 不参与 app 目标**已确认；如果手工给 ui 传 `-DEMBY_UI_BUILD_DEMO=ON` 并把它当子目录，会和 app main 冲突（CMake 默认 OFF，正常不会发生）。

## 8. 后续建议

1. Windows 实机：构建后放入 `libmpv-2.dll`，验证打开文件/文件夹、`--wid` 嵌入、D3D11VA、HDR/DV tone-map、mpv OSC 键位与 ASS 字幕。
2. 把 PlayerPage 的 mock 轨道列表替换为真实 `track-list`（或先做只读显示），并接通 sid/aid。
3. T6/后续：将 `emby::EmbyClient` 的数据映射到 `ui::MediaItem`，替换 mock 媒体库；播放地址通过 `http-header-fields` 传给 mpv。
4. 长时间运行/seek/切集/全屏回归测试，重点看 mpv 子窗口随 PlayerPage resize 的几何同步。
