# T17 播放模式接管 + 音轨过滤 + 输入转发

> 任务：task-17 · 负责人：build-verifier · 日期：2026-11-04
> 前置：T16 的 `ui::MainWindow::setPlayerMode()` / PlayerPage 铺满 + 浮层；T18 的顶层半透明 overlay + 自动隐藏。
> 范围：`app/src/mainwindow.*`、`app/src/videohost.*`、`app/src/mpvplayer.*`、`app/CMakeLists.txt`、本报告。未改 `app/src/ui/` 与 `app/src/emby/`。

## 1. 结论

- **播放接管整个窗口**：本地文件与 Emby 播放都会在 `PlayerController::showPlayerPage()` 调用 `m_window->setPlayerMode(true)`；返回/离开播放页时由 `backRequested` 与 stack `currentChanged` 调 `setPlayerMode(false)`，恢复 sidebar/topbar/statusbar/menu bar 与进入前页面。player mode 下菜单栏也隐藏（`updatePlayerWindowChrome()`），否则视频上方会留 24px 菜单栏。
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
- `updatePlayerWindowChrome()` 在 player mode / fullscreen 时隐藏 QMenuBar，退出时恢复；连接 `playerModeChanged`，并在 `QEvent::WindowStateChange` 时同步，保证 F11/全屏按钮路径也覆盖。
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
  MBTN_MID script-message app-mouse-probe
  ```
  mpv 收到后 app.log 输出：
  ```
  mpv mouse probe: MBTN_MID received in embedded window
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
- `setPlayerMode(true)`：sidebar/topbar/statusbar 全部隐藏，**menu bar 也隐藏**，PlayerPage 尺寸 == centralWidget 尺寸，top/bottom overlay `isWindow()==true`。
- `fullscreenRequested`：进入全屏后仍为 player mode。
- 合成 QMouseEvent 点返回按钮：全屏退出、player mode 保留。
- `setPlayerMode(false)`：sidebar/topbar/statusbar 恢复。

实测输出：

```
player overlays: top true bottom true backButton true
audio true mode true fill true overlayTopLevel true fullscreenOn true backExitedFullscreen true restore true
test_exit=0

# 菜单栏专项
player menu false height 0 page QSize(752, 704) central QSize(752, 704)
player mode: menu bar shown fullscreen false
after menu true
initial true playerHidden true fsHidden true exitFsHidden true restored true fsNonPlayerHidden true fsNonPlayerRestored true
```

### 6.3 Windows CI

- run `41862539535`（commit `db9fd51`，含 T17 输入转发/`SetWindowSubclass`/`comctl32`）**全绿**：Qt 6.8 MSVC 2022 x64 配置、Release 构建、Emby API 测试、windeployqt 打包、artifact 上传全部成功；artifact `emby-pc-win64` id `13341056549`（72,211,126 bytes）。
- 中间 run `41864472217`（`MBTN_MID` + 音轨 serial，commit `fafb03d`）也全绿。
- **最终 run `41867053696`（commit `88d1268`，含菜单栏隐藏 + 坐标宏）全绿**：配置/Release 构建/Emby API 测试/打包/artifact 上传全部成功；artifact `emby-pc-win64` id `13342717652`（72,211,822 bytes）。
- 结论：`SetWindowSubclass` + `comctl32` 的 Windows 编译/打包风险已排除；剩余验证是实机鼠标/键盘转发与 OSC 行为。

### 6.4 实机日志基线（用户此前提供）

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
   - 中键点击视频，检查 app.log 是否有 `mpv mouse probe: MBTN_MID ...`；
   - 按 F（或 F11/浮层全屏按钮）进全屏，按 Esc 应只退出全屏、不结束播放；
   - 全屏时点浮层“返回”应只退出全屏；再点一次才退出播放/恢复主界面。
3. 选一条 codec 为空的音轨片源：音轨菜单不应出现该坏轨道；若 mpv 回退，应看到“音轨 X 不可用，mpv 已回退到 Y”。
4. 若 OSC 出现，记录 OSC vs Qt 浮层哪个更稳；默认仍建议 Qt 顶层浮层。

## 9. 改动文件

| 文件 | 改动 |
|---|---|
| `app/src/mainwindow.cpp/.h` | player mode 进入/退出；全屏走 UI；返回按钮全屏拦截；音轨过滤 + 切换失败提示；Qt 键盘转发；鼠标信号转 mpv 命令 |
| `app/src/videohost.cpp/.h` | T22 起用 `WS_EX_TRANSPARENT` 鼠标穿透 + Qt 鼠标事件转信号；鼠标计数日志；子窗口几何同步 |
| `app/src/mpvplayer.cpp` | input.conf 中键探针；`app-mouse-probe` 日志 |
| `app/CMakeLists.txt` | WIN32 链接 `user32`（SetWindowLongPtr 设置扩展样式） |
| `reports/player-mode.md` | 本报告 |

## 10. T19 键盘焦点补丁（追加，2026-11-04）

> **T22 更正（2026-11-05）**：实机证明本节依赖的跨线程 `SetWindowSubclass` 实际失败（每 120ms 刷 `SetWindowSubclass failed ... error 0`，`mouse subclass hwnd=0x0 forwarded_events=0`），本节方案已由第 11 节 `WS_EX_TRANSPARENT` 鼠标穿透取代，仅保留历史记录。

### 10.1 问题

T17 的键盘转发只挂在 Qt 主窗口。实机点一下视频后，Windows 焦点会转移到 mpv 子 HWND；`--wid` 模式下 mpv 不处理自身输入，于是 `WM_KEYDOWN` 不进 Qt，空格/方向键/F/Esc 失效。

### 10.2 修复

- `VideoHost` 的 `SetWindowSubclass` 回调新增：
  - `WM_KEYDOWN/WM_SYSKEYDOWN`：首次按下转发（忽略 Windows 自动重复，按住产生的重复交给 mpv `keydown` 的自动重复）；
  - `WM_KEYUP/WM_SYSKEYUP`：转发 `keyup`；
  - `WM_CHAR`：debug 日志（字母/数字/空格已由 VK 路径转发，不重复发 `keypress`，避免空格双触发）；
  - `WM_SETFOCUS/WM_KILLFOCUS`：记录子窗口焦点变化（`EMBY_PC_DEBUG_INPUT=1` 时写 app.log）。
- VK→mpv 键名映射（`PlayerController::mpvKeyNameFromVirtualKey`，已作为 public static 供自测）：
  - `VK_SPACE→SPACE`、方向键、`VK_RETURN→ENTER`、`VK_ESCAPE→ESC`、Backspace/Delete/Insert/Home/End/PgUp/PgDn/Tab、`VK_F1-F10`；
  - A-Z→a-z、0-9、常用 `VK_OEM_*`（分号、等号、逗号、减号、句点、斜杠、反引号、方括号、反斜杠、单引号）；
  - Shift 前缀；Ctrl/Alt 留给 Qt。
- 焦点在子窗口时：
  - 普通键 → mpv `keydown`/`keyup`；
  - `VK_F11` → 直接 `PlayerPage::fullscreenRequested`（Qt 的 F11 快捷键收不到）；
  - `VK_ESCAPE` → mpv input.conf `ESC script-message app-exit-fullscreen`；Controller 收到后全屏则退全屏，否则退出 player mode（与 Qt Esc 行为一致）。
- 去重：`isDuplicateKeyForward(name, source)` 只抑制“不同来源”在 60ms 内的同名键；同一来源的自动重复放行，避免 Qt 长按方向键重复失效，也避免空格双触发。
- Qt 主窗口路径保持不变：焦点在 Qt 时继续 `keypress`；两条路径互斥（Windows 键盘只发给焦点窗口），去重兜底。

### 10.3 自测

- Linux Debug 构建通过；`-Wall -Wextra` 无新增 warning。
- offscreen 现有回归（菜单栏/全屏/播放模式/音轨过滤）全部通过。
- 新增 VK 映射单测（临时测试，未入库）：
  ```
  keyMapOk true
  initial true playerHidden true fsHidden true exitFsHidden true restored true fsNonPlayerHidden true fsNonPlayerRestored true
  exit=0
  ```
  覆盖 SPACE/LEFT/ESC/ENTER/f/5/,/Shift+f/修饰键空/F11 空。
- Windows CI：run `41872144136`（commit `0c8cfcb`）全绿，MSVC 2022 x64 的 Build (Release) 步骤通过（`WM_KEYDOWN` 子类化 + VK 映射编译无误）；artifact `emby-pc-win64` id `13346200280`（72,214,946 bytes）。
- Windows 实机待验证：点视频后按空格暂停、方向键 seek、F/F11 全屏、Esc 退全屏；`EMBY_PC_DEBUG_INPUT=1` 时 app.log 应出现 `VideoHost: native key down/up` 与 `forward key to mpv (native)`。



## 11. T22 输入转发修复：WS_EX_TRANSPARENT 鼠标穿透（2026-11-05）

### 11.1 实机证据与根因

- 用户日志：app.log 每 120ms 刷一次
  `VideoHost: SetWindowSubclass failed for mpv child 853554 error 0`；
  diagnostics：`mouse subclass hwnd=0x0 forwarded_events=0`。
- 结论：T19 的 `SetWindowSubclass` 没生效。mpv 的 VO 子窗口在 mpv 自己的线程创建，跨线程子类化通常直接失败；`error 0` 是 `SetWindowSubclass` 失败时未设置 `GetLastError` 的典型表现。转发计数始终为 0，鼠标/键盘实际都没走 T19 路径。

### 11.2 修复方案（A）

- 删除 `SetWindowSubclass/RemoveWindowSubclass/DefSubclassProc` 路径和每 120ms 失败日志；删除 `comctl32` 链接。
- `VideoHost::syncMpvChildGeometry()`（120ms 定时 + resize/show）对 mpv 直接子 HWND 设置：
  `WS_EX_TRANSPARENT | WS_EX_NOACTIVATE`。
  - 鼠标命中测试跳过 mpv 子窗口，事件落到父级 Qt `VideoHost`。
  - `WS_EX_NOACTIVATE` 防止点击激活 mpv 子窗口。
  - 若 mpv 重建/重置窗口样式，定时器会重新检查并补上；只在样式变化时打一条 `VideoHost: mouse passthrough enabled on mpv child ... exstyle 0x...`，不再刷屏。
- `VideoHost` 开启 `setMouseTracking(true)`，重写 Qt 鼠标事件：
  - move → `mouseMoved`；左/中/右按下 → `mouseButtonPressed(0/1/2)`；双击 → `doubleClick=true`；wheel → `mouseWheel(3/4)`；
  - 坐标按 `devicePixelRatioF()` 从 Qt 逻辑坐标换算成 mpv 物理像素；
  - 点击时 `window()->activateWindow() + setFocus(Qt::MouseFocusReason)`，焦点回到 Qt 主窗口，键盘继续走 Qt `eventFilter` 的 `keypress` 路径。
- PlayerController 保留 Qt 鼠标信号 → mpv `mouse` 命令（原 T17 逻辑）；删除 T19 native keydown/keyup 连接和 `mpvKeyNameFromVirtualKey/forwardNativeKeyToMpv` 死代码。

### 11.3 自测

- Linux Debug 构建通过；`-Wall -Wextra` 无新增 warning。
- offscreen `VideoHost` 鼠标测试：
  ```text
  moves 1 lastMove QPoint(10,12)
  buttons 2 lastButton QPoint(20,30) double true
  releases 0 wheels 1 lastWheel QPoint(40,50)
  hasSubclassWord false
  exit=0
  ```
  覆盖 move / left press / double click / wheel，坐标按 DPR=1 原样；诊断中无 subclass。
- `grep SetWindowSubclass/RemoveWindowSubclass/DefSubclassProc`：只剩注释，无调用代码。

### 11.4 Windows CI

- run `41952931196`（commit `81c2751`）全绿；MSVC 2022 x64 Build (Release) 通过，`WS_EX_TRANSPARENT`/Qt 鼠标事件代码编译无误；artifact `emby-pc-win64` id `13382555893`（72,221,356 bytes）。

### 11.5 实机待验证

1. 播放视频后移动鼠标：控制条出现/保持，不再需要点击才刷新。
2. 点击视频左键：暂停/继续；双击：全屏（按 input.conf 绑定）；右键：菜单；滚轮：音量/seek（按 input.conf）。
3. 点击视频后按空格/方向键/F/F11/Esc：应正常，焦点已回 Qt。
4. app.log 应只出现一次（或每次重建一次）`mouse passthrough enabled on mpv child ... exstyle ...`，**不再有** `SetWindowSubclass failed` 刷屏；diagnostics 的 `mouse passthrough hwnd=0x... forwarded_events=N` 应随鼠标操作增长。
5. 若某些 Windows/mpv 版本仍不穿透：回退方案 B（`WH_MOUSE_LL` 低级钩子或每帧重设样式）已在任务描述中，可按日志中 `SetWindowLongPtr ... failed` 定位。


## 12. T32 全屏按钮/上下集按钮排查（2026-11-05）

### 12.1 全屏按钮链路

现在 FullscreenButton 的全链路为：

```text
QPushButton::clicked
  -> PlayerPage::fullscreenRequested
  -> ui::MainWindow::toggleFullscreen
  -> PlayerPage::setFullscreen + QWidget::showFullScreen()
```

- PlayerPage 按钮日志：`PlayerPage: FullscreenButton clicked fullscreen=... playerMode=... windowFullScreen=...`
- MainWindow 入口/出口日志：`MainWindow::toggleFullscreen enter isFullScreen=... m_fullscreen=... m_playerMode=... geometry=...`
- PlayerPage::setFullscreen 日志记录目标状态和窗口真实全屏状态。
- **兜底修复**：按钮发出 `fullscreenRequested` 后，在下一轮事件循环检查窗口真实状态；若状态没有变化（说明 UI 层连接缺失/断开/被异常拦截），通过 `QMetaObject::invokeMethod(window(), "toggleFullscreen")` 直接调用 MainWindow 的 `Q_INVOKABLE` 槽一次。正常路径已改变状态时不会重复切换。
- Windows app.log 排查顺序：先看 `PlayerPage: FullscreenButton clicked`，再看 `MainWindow::toggleFullscreen enter`；若只有第一条没有第二条，说明信号连接问题；若两条都有但 `isFullScreen` 仍 false，检查 WindowFrame/窗口状态。

### 12.2 上一集/下一集

- `PlayerPage::setEpisodeNavigationEnabled(hasPrevious, hasNext)`：
  - 首集：上一集按钮 disabled，tooltip「没有上一集」；
  - 末集：下一集按钮 disabled，tooltip「没有下一集」；
  - 空队列：两个按钮均 disabled；初始构造即 disabled，避免未开始播放时误点。
- PlayerPage 的 Prev/Next clicked 日志记录 enabled/playerMode；信号仍保持 `previousEpisodeRequested/nextEpisodeRequested`。
- UI `MainWindow::stepEpisode` 只处理 `m_mockMode` 的 mock 数据切换；真实播放不再被 UI mock 连接和 PlayerController 的 Emby 队列连接双重处理。
- `PlayerController::stepEmbyEpisode/playEpisodeAt/loadEpisodeQueue` 日志包含 `m_embyItemId`、`m_episodeQueue.size()`、`m_episodeIndex`、目标 index；队列加载、切集和清空时同步按钮状态。
- 本地播放列表：`playFiles` 根据文件数量/起始 index 设置按钮状态；Emby movie（无剧集队列）禁用 Prev/Next。

### 12.3 自测

- `emby-ui-demo --selftest` **190 项 ALL PASS**（其中 T32 新增：FullscreenButton 点击链路/再次退出全屏、队列为空时 Prev/Next disabled+tooltip）。
- 全屏测试输出可见完整日志：
```text
PlayerPage: FullscreenButton clicked ...
MainWindow::toggleFullscreen enter ...
MainWindow::toggleFullscreen enter-fullscreen ...
```
- 不依赖真实服务器；Windows 实机仍需按下方日志确认。

### 12.4 Windows 实机复测

1. 播放任意条目，点控制条全屏按钮：应进入全屏；再点退出。
2. 检查 app.log 是否按顺序出现上述三条日志；若 fallback 生效会出现 `PlayerPage: fullscreenRequested did not change window state; invoking MainWindow::toggleFullscreen fallback`。
3. 播放剧集第一集：上一集按钮应灰色、tooltip「没有上一集」；最后一集下一集同理；中间集两个可用。
4. 播放电影：Prev/Next 均应 disabled；点击不应改变播放状态。
5. 连续点上一集/下一集，检查 app.log 的 `PlayerController::stepEmbyEpisode ... queue=... index=... target=...`，目标 index 应在 0..size-1 内。

## 13. T36 上一集/下一集：不依赖播放起点，存在即可点

> 任务：task-36 · 负责人：build-verifier · 日期：2026-11-05
> 范围：app/src/mainwindow.h/.cpp（PlayerController）、app/src/ui/playerpage.h/.cpp（沿用 T32 API），本报告。

### 13.1 实现

- `PlayerController::playEmbyItem()` 每次开始 Emby 播放都调用 `requestEpisodeSiblings(itemId)`；如果当前 item 已在队列中（详情页/继续观看/上一集路径），只同步按钮，不发请求。
- 单集来源（首页卡片、继续观看、单集点播）异步流程：
  1. `EmbyClient::fetchItem(episodeId)` 取 `type/seriesId/seasonId/parentId`；
  2. 非 Episode（Movie/Series）→ 两个按钮 disabled；
  3. Episode → 查 `m_episodeCache`（同一 Series/Season 5 分钟）；
  4. 缓存未命中：`fetchItems` 查询 `ParentId=seriesId (recursive)` 或 `ParentId=seasonId`，`IncludeItemTypes=Episode`，按季/集号排序，limit 1000；
  5. `applyEpisodeEntries()` 按当前 itemId 找 index，设置 `setEpisodeNavigationEnabled(index>0, index+1<count)`；
  6. 缓存结果 5 分钟；切号/清会话时清空缓存。
- 点击 next/prev 仍走 T32 的 `stepEmbyEpisode/playEpisodeAt`；队列为空时先异步请求并提示「正在获取剧集列表…」，不直接判死。
- 请求失败时保守允许按钮点击（点击后重试/提示），成功拿到 Movie 类型时禁用。
- 日志：`PlayerController::requestEpisodeSiblings series/season/item`、`episodeSiblings cache hit`、`episodeSiblings series/season/current/count/index/prev/next`、`episodeSiblings not episode type`。

### 13.2 行为

| 场景 | prev | next |
|---|---|---|
| 5 集中间集 | enabled | enabled |
| 第一集 | disabled | enabled |
| 最后一集 | enabled | disabled |
| 电影/本地单文件 | disabled | disabled |
| 请求失败/未知 | 保守 enabled（点击重试） | 保守 enabled |

### 13.3 缓存

- Key：seriesId；无 seriesId 时 seasonId；TTL 5 分钟。
- 缓存保存完整 `EpisodeEntry`（含 startPositionTicks），从任意集进入后 next/prev 可续播各自进度。

### 13.4 自测/验收

- Linux app Debug 构建通过；`-Wall -Wextra` 无新增 warning；`emby-ui-demo --selftest` 基线 155 项保持。
- 假服务器建议用例（Windows/本地 fake Emby）：某 Series 5 集，分别从第 1/3/5 集进入，检查 `setEpisodeNavigationEnabled` 的 prev/next 与 app.log 的 index/count；Movie 进入检查 disabled。
- Windows 实机：从首页/继续观看直接点某一集，控制条上一集/下一集应按兄弟集列表启用；点下一集直接切到对应 episode id 并上报 Progress/Stopped。

## 14. T46 播放模式拖动窗口 + 全屏退出复位

> 任务：task-46 · 负责人：build-verifier · 日期：2026-11-06

### 14.1 播放模式拖动

- `VideoHost`/`VideoArea` 与顶部浮层空白区左键按下后，移动超过 4px 调用
  `window()->windowHandle()->startSystemMove()`；单击/双击仍走原 mpv 点击路径。
- 5px 边缘缩放优先：命中边缘时仍走 T35 `startSystemResize`，不会误触拖动。
- 全屏状态禁用拖动。日志：
  `PlayerPage::windowDrag source=video/topBar press=... current=... distance=... startSystemMove=...`。
- 单测：4px 不触发、5px 触发、全屏禁用、对角距离；offscreen 共 22 项 PASS。

### 14.2 全屏退出状态

- 进入全屏前记录 `normalGeometry`/wasMaximized；退出时：
  `showNormal()` → `setWindowState(Qt::WindowNoState)` → 恢复窗口几何；
  **即使进入前是最大化，也回普通窗口**，避免最大化状态没有边缘缩放光标。
- `setPlayerMode(false)` 的全屏退出分支与 `toggleFullscreen()` 共用
  `restoreWindowedStateFromFullscreen()`。
- 退出后 `QTimer::singleShot(0)` 调 `PlayerPage::refreshOverlayGeometry()`，刷新顶层浮层几何。
- 日志：`restoreWindowedStateFromFullscreen ... normalGeometry=... isMaximized=false windowState=0`。
- 单测断言：退出全屏 `isFullScreen=false`、`windowState()==NoState`、`isMaximized()=false`、恢复尺寸。

### 14.3 待实机

- 播放模式视频中央/顶部栏空白处拖动；日志应出现 startSystemMove true。
- 全屏退出后主页四边/四角缩放光标；日志 windowState=0。

## 15. T51 控制条点击命中 / Seek / 弹窗恢复

- **按钮扩展命中**：`PlayerControlBar` 层统一分发，按钮 rect 外扩 6px；按下落在按钮边缘/间隙时直接 `click()`，保留正常 QPushButton 事件。
- **SeekSlider**：视觉轨道仍 4px，Widget 命中高度 28px；点击/拖动任意位置直接算值，发出 `seekRequested`，同时更新当前时间；拖动期间暂停自动隐藏计时。
- **日志**：`PlayerPage: control mouse ... watched/objectName/local/global`、`button clicked`、`SeekSlider click value=...`、`extended hit -> ...`。
- **弹窗/菜单**：`QMenu::exec` 关闭后 `showOverlays()+raise()`、重置 interaction 计时并重新启动自动隐藏；统计按钮点击也重置计时，避免关闭 mpv OSD 后浮层立刻被隐藏。
- **输入属性**：`showOverlays()` 显式清除 `WA_TransparentForMouseEvents`，保持 `WindowDoesNotAcceptFocus + WA_ShowWithoutActivating`（鼠标仍可点，避免抢播放键盘焦点）。
- **回归**：新增 3 条 offscreen 断言——边缘扩展点击触发 `playPauseRequested`、SeekSlider 任意位置点击发 `seekRequested`、菜单关闭后控制条可见且非透明输入；总计 **190 项 ALL PASS**。
- 待实机确认：控制条图标边缘/间隙、进度条任意位置、统计 toggle 后立即点其他按钮。

## 16. T53 控制条默认图标模式

- 字幕/音轨/统计按钮不再显示文字，始终 `setText(QString())`；当前轨道信息放 tooltip（如 `字幕：中文（简体）`、`音轨：中文 5.1`）。
- 960 图标化逻辑保留为统一的纯图标布局；按钮尺寸/间距/hover/pressed/focus 沿用 T45/T51 token。
- 播放/上下集/音量/全屏本就为图标，无文字残留。
- 回归：`emby-ui-demo --selftest` **190 项 ALL PASS**（T53 断言字幕/音轨/统计 text 为空且 tooltip 非空）。
- 截图：`preview/05-player.png`、`10-player-960x600.png`、`12/13` 已更新。

## 17. T55 控制条/顶栏不透明命中区

- 根因：顶层 overlay 使用 `WA_TranslucentBackground` 分层窗口，Windows 按像素 alpha 命中；按钮透明区域/渐变透明区域鼠标穿透，只有图标不透明中心能 hover/点击。
- 修复：取消 `WA_TranslucentBackground`、`WA_NoSystemBackground`，设置 `autoFillBackground`；QSS `PlayerTopBar`/`PlayerControlBar` 改为不透明 `#0B1220` + `#1E293B` 分隔线。
- 按钮：字幕/音轨/统计固定 **36×36** 整块可点，图标 18px，hover/pressed 覆盖整个按钮；播放 48px、上下集/返回/全屏 36px。
- 保留：自动隐藏、T51 扩展命中、T53 纯图标 tooltip、T46 拖动/全屏、T47 sidebar 恢复。
- 回归：新增 4 条断言——顶栏/控制条非 translucent、grab 像素 alpha=255、控制条四角按钮命中 4/4；总计 **190 项 ALL PASS**。
- 截图：`preview/05-player.png`、`10-player-960x600.png`、`12/13` 更新为不透明控件条。

## 15. T56 播放画面全铺满（去掉 5px 黑边）

> 任务：task-56 · 负责人：build-verifier · 日期：2026-11-06

- `applyVideoMargins()` 在播放模式/全屏统一 `setContentsMargins(0,0,0,0)`，不再保留 T31 的 5px。
- 顶层/页面内 overlay 同步去掉 margin：顶栏距左上 12/10，控制条贴底铺满；全屏无边框。
- 边缘缩放改为 `VideoHost/PlayerPage` 的 **6px 命中区**：`m_surface/m_surfaceStack` 已安装
  `PlayerPage` 事件过滤器，左键在边缘先走 `startSystemResize`，其余继续 T46 空白区拖动/mpv 点击。
- `kPlayerResizeHit` 取代 layout 用的 `kPlayerResizeMargin`；全屏命中为空（不缩放）。
- 单测：video area == page rect（无 5px）、1/5px 命中、6px 不命中、四角命中、全屏禁用、普通模式铺满；
  共 7 项 PASS；`emby-ui-demo --selftest` 190 PASS。
- 实机确认：播放四边/四角可缩放且无黑边；全屏无边框。
