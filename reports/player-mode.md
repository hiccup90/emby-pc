# T17 播放模式接管 + 音轨过滤 + 输入转发

> 任务：task-17 · 负责人：build-verifier · 日期：2026-11-04
> 前置：T16 的 `ui::MainWindow::setPlayerMode()` / PlayerPage 铺满 + 浮层；T18 的顶层半透明 overlay + 自动隐藏。
> 范围：`app/src/mainwindow.*`、`app/src/videohost.*`、`app/src/mpvplayer.*`、`app/CMakeLists.txt`、本报告。未改 `app/src/ui/` 与 `app/src/emby/`。

## 1. 结论

- **播放接管整个窗口**：本地文件与 Emby 播放都会在 `PlayerController::showPlayerPage()` 调用 `m_window->setPlayerMode(true)`；返回/离开播放页时由 `backRequested` 与 stack `currentChanged` 调 `setPlayerMode(false)`，恢复 sidebar/topbar/statusbar 与进入前页面。
- **VideoHost 铺满**：T16 的 PlayerPage 已把 `VideoArea` 作为唯一布局项；T17 只在进入 player mode 后确认 `PlayerPage == 主窗口客户区`，并把 VideoHost 挂到 `videoSurfaceContainer()`。offscreen 实测 `page 752x680 == central 752x680`。
- **全屏/返回**：不再由 PlayerController 直接 `showFullScreen()`；mpv 内的 F/Esc 转成 `PlayerPage::fullscreenRequested`，复用 `ui::MainWindow::toggleFullscreen()`，保证 `m_fullscreen`/chrome/stack 同步。全屏时点返回按钮会先退出全屏（事件过滤器拦截），不会直接退出播放。
- **音轨过滤**：`audioTracksFromTrackList()` 过滤 `type != audio` 以及 `codec` 为空/ `none` 的轨道；实机 aid=1（codec 空）不再出现在菜单。切换 aid 后 900ms 核对实际 aid，mpv 回退时提示“音轨不可用，已回退到 …”。
- **输入转发（mpv issue #4795）**：`--wid` 下 mpv 禁用自身窗口输入。VideoHost 用 `SetWindowSubclass` 子类化 mpv 子 HWND，把鼠标移动/左中右键/滚轮转成 mpv `mouse x y [button] [double]`；Qt 主窗口把键盘转成 `keypress`。OSC 仍由 `EMBY_PC_MPV_OSC=1` 可选启用，并新增中键探针写入 app.log。
- **OSC vs Qt 顶层浮层（结论）**：**当前以 T18 的 Qt 顶层半透明浮层为主**。它不依赖 libmpv 是否带 osc.lua、不依赖 `--wid` 的输入转发，跨版本/跨发行包更稳；OSC 作为 `EMBY_PC_MPV_OSC=1` 的排障/可选路径。输入转发修好后 OSC 理论上可用，但本容器没有 Windows/GPU/libmpv DLL，必须在 Windows 实机确认；若实测 OSC 稳定，可再评估切换，默认不切。

## 2. 播放模式接线

### 2.1 进入

`PlayerController::showPlayerPage()`（本地播放与 Emby PlaybackInfo 成功后都会走到）：

```cpp
if (!m_window->isPlayerMode())
    m_window->setPlayerMode(true);
ensurePlayerControlsVisible();
...
```

- T16/T18 的 `setPlayerMode(true)` 负责：隐藏 sidebar/topbar/statusbar、stack 切到 `StackPlayer`、`PlayerPage::setPlayerModeActive(true)`（启动顶层 overlay + 自动隐藏）。
- `ensurePlayerControlsVisible()` 只调用 `m_playerPage->setControlsVisible(true)`；**不再**直接 `PlayerControlBar->show()/raise()`（T18 浮层是独立顶层窗口，显隐由 PlayerPage 统一管理）。

### 2.2 退出

- `PlayerPage::backRequested` 的 UI 槽在 player mode 下先退出全屏（如需要）并 `setPlayerMode(false)`；Controller 侧同时清理 Emby 会话，并防御性再调一次 `setPlayerMode(false)`。
- ContentStack `currentChanged`：离开 PlayerPage 时停止 mpv、清理 Emby 会话，并在仍处于 player mode 时恢复主界面。这样从任何路径离开播放页都不会停在“无 chrome”状态。

### 2.3 浮层

T18 的 `PlayerTopBar` / `PlayerControlBar` 是独立顶层半透明窗口（`isWindow()==true`），定位用 `mapToGlobal`，自动隐藏由 PlayerPage 的 `QTimer` + `QCursor::pos()` 管理。T17 只：

- 在构造时找到 `PlayerBackButton` 并安装全屏返回拦截过滤器；
- 不再调用 overlay 的 `show()/raise()`，避免与自动隐藏/顶层窗口状态打架。

## 3. 全屏与返回行为

- 删除 `PlayerController::setFullscreen()`（原实现直接 `showFullScreen()/showNormal()`，会让 UI 的 `m_fullscreen` 不同步）。
- mpv 的 `toggleFullscreenRequested` / `exitFullscreenRequested` 改为：
  ```cpp
  QMetaObject::invokeMethod(m_playerPage, "fullscreenRequested", Qt::DirectConnection);
  ```
  即走 `ui::MainWindow::toggleFullscreen()`。
- Qt 的 Esc/F11 快捷键保留：Esc 在全屏时先退出全屏；非全屏才退出 player mode。
- **返回按钮全屏拦截**：`PlayerController` 在 `PlayerBackButton` 上安装 eventFilter；全屏时拦截 `MouseButtonRelease`，只触发 `fullscreenRequested`（退出全屏），不让按钮发出 `backRequested`，所以第一次返回不会直接结束播放。offscreen 测试已用合成 QMouseEvent 验证。
- F 键：Qt 主窗口转发 `keypress f` 给 mpv；mpv input.conf 的 `f script-message app-toggle-fullscreen` 回到 Controller，再走 UI toggle；与 F11/Qt 按钮路径一致。

## 4. 音轨过滤（实机 aid=1 codec 空）

新增 public static（便于单测）：

```cpp
QList<ui::TrackInfo> PlayerController::audioTracksFromTrackList(
    const QString &json, int *filteredCount = nullptr);
```

规则：

- 只保留 `type == "audio"` 且 `id > 0`；
- `codec.trimmed()` 为空，或等于 `none`（大小写不敏感）→ 过滤，`filteredCount++`；
- `refreshTracks()` 用该函数生成 audio 菜单；被过滤数量写 app.log：
  `track-list: hidden N audio track(s) with empty/none codec`。

切换失败提示：

- `setPropertyString("aid", id)` 返回 false → 立即提示“音轨 X 切换失败”；
- 返回 true 后延迟 900ms 读实际 `aid`，若与请求不同 → `qWarning` + 状态栏“音轨 X 不可用，mpv 已回退到 Y”，并刷新菜单。

## 5. 输入转发（mpv --wid 输入被禁用）

mpv 官方 issue #4795 确认：`--wid` 嵌入时 mpv 会禁用自己窗口的输入，所以 OSC 不显示、mpv 键位不生效。T17 增加宿主转发：

### 5.1 鼠标（VideoHost）

- `VideoHost::syncMpvChildGeometry()` 在同步 mpv 直接子窗口几何后，对第一个直接子 HWND 调用 `SetWindowSubclass()`（保留原 EnumChildWindows 同步逻辑）。
- 子类化回调捕获：
  - `WM_MOUSEMOVE` → `mouse x y`
  - `WM_LBUTTONDOWN` → `mouse x y 0`
  - `WM_LBUTTONDBLCLK` → `mouse x y 0 double`
  - `WM_RBUTTONDOWN` → `mouse x y 2`
  - `WM_MBUTTONDOWN` → `mouse x y 1`
  - `WM_MOUSEWHEEL` → 屏幕坐标转客户区，按 120 的倍数发 `mouse x y 3/4`（mpv: 3=WHEEL_UP，4=WHEEL_DOWN）
  - 左/中/右键 up 只做计数/日志，不重复发 click（避免 OSC 双触发）
- 坐标先用 mpv 子窗口客户区像素；如 Windows 高 DPI 下 OSC 命中偏移，再按 DPI 换算（当前按需求先保持像素）。
- `WM_NCDESTROY` 自动 `RemoveWindowSubclass` 并清空句柄；VideoHost 析构时也会移除。
- 为 `SetWindowSubclass` 在 `app/CMakeLists.txt` 的 WIN32 链接增加 `comctl32`。

### 5.2 键盘（Qt 主窗口）

- `PlayerController` 在 `QCoreApplication` 上安装 eventFilter；player mode + 主窗口 active + mpv ready 时，把 `QKeyEvent` 映射成 mpv key name 并执行 `keypress <name>`。
- 覆盖 Space/方向键/PageUp/PageDown/Home/End/Enter/Backspace/Delete/Insert/Tab、A-Z、0-9、F1-F10 及 Shift 组合；Ctrl/Alt/Meta 留给 Qt；**Esc/F11 不转发**，避免和 Qt 快捷键双触发。
- `EMBY_PC_DEBUG_INPUT=1` 时 app.log 记录 `forward key to mpv <name>`。

### 5.3 OSC 验证

- `EMBY_PC_MPV_OSC=1` 时 mpv `osc=yes`；VideoHost 在 `EMBY_PC_MPV_OSC=1` 下记录鼠标事件计数（首次 + 每 20 次）。
- input.conf 新增**中键探针**（避免覆盖 OSC 左键点击）：
  ```
  MBTN_MIDDLE script-message app-mouse-probe
  ```
  mpv 收到后 app.log 输出：
  ```
  mpv mouse probe: MBTN_MIDDLE received in embedded window
  ```
  这条日志能端到端验证：VideoHost 子类化 → mouse 命令 → mpv input handler → client message。若没有这条日志，说明鼠标消息/转发链路有问题；若有，OSC 左键/移动也应能收到同类事件。

## 6. 验证

### 6.1 构建

```bash
cd /root/github/emby-pc/app
./scripts/build-linux.sh
# ==> OK: app/build-linux/emby-pc
```

额外 `-Wall -Wextra` 构建通过，无新增 warning。

### 6.2 offscreen 行为测试

临时测试（链接 app 源码，未入库）覆盖：

- 假 `track-list`：audio id=1 codec 空、id=2 aac、id=3 sub、id=4 audio codec none；期望 1 条可用音轨（id=2），过滤 2 条。
- `setPlayerMode(true)`：sidebar/topbar/statusbar 全部隐藏，PlayerPage 尺寸 == centralWidget 尺寸，top/bottom overlay `isWindow()==true`。
- `fullscreenRequested`：进入全屏后仍为 player mode。
- 合成 QMouseEvent 点返回按钮：全屏退出、player mode 保留。
- `setPlayerMode(false)`：sidebar/topbar/statusbar 恢复。

实测输出：

```
player overlays: top true bottom true backButton true
audio true mode true fill true overlayTopLevel true fullscreenOn true backExitedFullscreen true restore true
test_exit=0
```

### 6.3 实机日志基线（用户此前提供）

- Windows 11 24H2、DPR 2、mpv v0.41.0-1434-g46787be25、gpu-api=d3d11、hwdec=d3d11va、`options/osc=no`、`input-vo-keyboard=yes`、`input-cursor=yes`。
- 该日志中 PlayerPage 960x557、PlayerControlBar 960x90，说明 T16 前浮层仍是页面内控件；T18 后改为顶层窗口，T17 已按顶层窗口接线。

## 7. 本环境限制 / 待实机确认

- 容器没有 Windows、GPU、真实 libmpv-2.dll，无法实际验证 `--wid` 子窗口、`SetWindowSubclass`、D3D11VA、OSC 显示与鼠标命中；Linux 只能验证编译、逻辑和 offscreen 行为。
- Windows CI 需要验证 `SetWindowSubclass` + `comctl32` 的 MSVC/MinGW 编译与打包；由 release-engineer 提交后跑 CI。
- 顶层 overlay 的半透明/自动隐藏在 Windows 由 T18 的顶层窗口实现；T17 不再直接 show/raise，避免状态冲突。
- 返回按钮全屏拦截只针对真实鼠标释放；键盘/其它路径的 `backRequested` 仍走 UI 的“先退出全屏再退出播放”逻辑。

## 8. 实机重测步骤

1. 设置 `EMBY_PC_MPV_OSC=1`、`EMBY_PC_DEBUG_INPUT=1`（可选），启动新 artifact。
2. 播放本地文件或 Emby 条目：
   - 主窗口应只剩视频（sidebar/topbar/statusbar 消失），PlayerPage 铺满客户区；
   - 移动鼠标，T18 浮层应出现并在 2.5s 后自动隐藏；
   - 中键点击视频，检查 app.log 是否有 `mpv mouse probe: MBTN_MIDDLE ...`；
   - 按 F（或 F11/浮层全屏按钮）进全屏，按 Esc 应只退出全屏、不结束播放；
   - 全屏时点浮层“返回”应只退出全屏；再点一次才退出播放/恢复主界面。
3. 选一条 codec 为空的音轨片源：音轨菜单不应出现该坏轨道；若 mpv 回退，应看到“音轨 X 不可用，mpv 已回退到 Y”。
4. 若 OSC 出现，记录 OSC vs Qt 浮层哪个更稳；默认仍建议 Qt 顶层浮层。

## 9. 改动文件

| 文件 | 改动 |
|---|---|
| `app/src/mainwindow.cpp/.h` | player mode 进入/退出；全屏走 UI；返回按钮全屏拦截；音轨过滤 + 切换失败提示；Qt 键盘转发；鼠标信号转 mpv 命令 |
| `app/src/videohost.cpp/.h` | `SetWindowSubclass` 子类化 mpv 子 HWND；鼠标消息转信号；鼠标计数日志；子窗口销毁清理 |
| `app/src/mpvplayer.cpp` | input.conf 中键探针；`app-mouse-probe` 日志 |
| `app/CMakeLists.txt` | WIN32 链接 `comctl32`（SetWindowSubclass） |
| `reports/player-mode.md` | 本报告 |
