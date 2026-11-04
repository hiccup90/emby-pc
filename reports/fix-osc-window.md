# T15 实机问题修复：播放无控制条（OSC 未显示）+ 主窗口不可缩放

> 任务：task-15 · 负责人：build-verifier · 日期：2026-11-04
> 用户实机反馈基线：artifact `emby-pc-win64`，commit `a47403d`（后续 T7 已合入，本修复基于当前工作区）。
> 范围：`app/src/mainwindow.*`、`app/src/videohost.*`、`app/src/mpvplayer.*`、`app/src/ui/playerpage.*`、本报告。

## 1. 结论

- **控制条**：根因是 PlayerController 在本地/Emby 播放开始时显式调用 `PlayerControlBar->hide()`；同时 libmpv 的 OSC 在部分 Windows 构建/嵌入模式下不加载或不可见。已删除隐藏逻辑，改为 **Qt PlayerControlBar 作为主控制条**；OSC 改为可选（环境变量 `EMBY_PC_MPV_OSC=1`），不再依赖 OSC。
- **窗口缩放**：`ui::MainWindow` 构造时 `setMinimumSize(960, 600)`，默认 `1280x760`，且状态栏 size grip 被关闭；在 1366x768/高 DPI 小屏上容易表现为“不能调整/超出屏幕”。PlayerController 现在把最小尺寸放宽到 **720x480**、启用右下角 resize grip、按屏幕可用区域收紧初始尺寸，并记录 resize 事件。
- **mpv 子窗口跟随**：VideoHost 从“只找第一个子 HWND”改为 `EnumChildWindows` 枚举 + 直接子窗口同步尺寸，覆盖主窗口缩放/全屏后的 mpv VO 窗口。
- **诊断增强**：app.log 记录窗口尺寸/屏幕/控制条可见性/mpv 版本与关键选项/VideoHost 子窗口几何；帮助菜单新增 **“导出诊断信息…”**，一键打包 app.log、mpv.log 与诊断文本。

## 2. 根因与修复

### 2.1 播放时没有控制条

**根因（代码层）**：
- `app/src/mainwindow.cpp` 的 `playFiles()` 和 `handlePlaybackInfo()` 在开始播放后执行：
  ```cpp
  if (auto *controlBar = m_playerPage->findChild<QWidget *>("PlayerControlBar"))
      controlBar->hide();
  ```
- 同时 `MpvPlayer` 设置 `osc=yes`，但 libmpv 的 builtin scripts（osc.lua/stats.lua）在部分发行包中可能被裁剪，`--wid` 嵌入模式下 OSC 也可能不加载/不接收鼠标；因此用户既看不到 OSC，也看不到被隐藏的 Qt 控制条。

**修复**：
- 删除两处 `PlayerControlBar->hide()`。
- 新增 `PlayerController::ensurePlayerControlsVisible()`：调用 `PlayerPage::setControlsVisible(true)`，并对控制条 `show()/raise()`；在 `playFiles`、`handlePlaybackInfo`、`showPlayerPage`、`MpvPlayer::fileLoaded` 时都确保可见。
- `PlayerPage::attachVideoSurface()` 也先显示顶栏/控制条并 `raise()`，防止后续 UI 状态把控制条隐藏。
- mpv 侧：默认 `osc=no`（Qt 控制条为主），`load-scripts=yes` 保留（stats 等脚本仍可加载）；`input-vo-keyboard=yes`、`input-cursor=yes` 显式设置，确保嵌入窗口可接收鼠标/键盘。
- 需要排查 OSC 时：设置环境变量 `EMBY_PC_MPV_OSC=1`，mpv 会 `osc=yes` 并启用 OSC 的 script-opts；Qt 控制条仍然保留，避免再次“全无控制条”。

### 2.2 主窗口不可缩放

**根因（代码层）**：
- `app/src/ui/mainwindow.cpp:79`：`setMinimumSize(960, 600)`；
- 构造时 `resize(1280, 760)`，在 1366x768/高 DPI 小屏上可能超出可用区域；
- `statusBar()->setSizeGripEnabled(false)`，右下角没有可见 resize grip；
- 播放时若 mpv 子 HWND 尺寸不跟随，用户也容易把“视频区不跟随”感知为“窗口不可缩放”。

**修复（不改 ui 源码，避免越界）**：
- PlayerController 构造时：
  - `setMinimumSize(720, 480)`（覆盖 UI 的 960x600）；
  - `setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX)`；
  - `statusBar()->setSizeGripEnabled(true)`；
  - 按 `QScreen::availableGeometry()` 收紧初始尺寸（留 48x96 边距，且不小于 720x480）。
- 安装 `eventFilter` 记录 `QEvent::Resize`（500ms 节流），便于确认窗口是否真的收到缩放事件。
- `VideoHost::syncMpvChildGeometry()`：
  - 使用 `EnumChildWindows` 枚举 mpv 子窗口；
  - 只同步直接子窗口（mpv VO）到 host client rect，避免误拉伸 OSC/统计等内部窗口；
  - 保留 120ms 定时器，覆盖窗口 resize/全屏/布局变化；
  - `EMBY_PC_DEBUG_GEOMETRY=1` 时输出每次 resize/show 与子窗口同步日志。

### 2.3 诊断信息

- `app.log` 新增：
  - `window: minimum ... current ... sizeGrip ...`
  - `window: screen available ... dpr ...`
  - `window resize event old -> new ...`
  - `mpv state [initialized|file-loaded]: mpv-version/vo/gpu-api/gpu-context/hwdec/hwdec-current/options-osc/load-scripts/input-vo-keyboard/input-cursor/options-wid/script-opts/script-list`
  - `VideoHost: geometry ... nativeWindow ...` 及子 HWND class/rect/visible
  - `controls: PlayerControlBar visible ... geometry ...`
- 帮助菜单新增 **“导出诊断信息…”**：写入 `%LOCALAPPDATA%\emby-pc\emby-pc\diagnostics-<时间>\`，包含 `diagnostics.txt`、`app.log`、`mpv.log`，并打开该目录。

## 3. 改动文件

| 文件 | 改动 |
|---|---|
| `app/src/mainwindow.cpp/.h` | 删除控制条隐藏；确保控制条可见；窗口最小尺寸/初始尺寸/size grip；resize 日志；mpv/VideoHost 诊断日志；帮助菜单导出诊断 |
| `app/src/mpvplayer.cpp` | OSC 默认关闭、`EMBY_PC_MPV_OSC=1` 可选；`input-cursor=yes`；初始化日志 |
| `app/src/videohost.cpp/.h` | EnumChildWindows 同步直接子窗口；resize/show 日志；`diagnosticSummary()` |
| `app/src/ui/playerpage.cpp` | `attachVideoSurface()` 时强制显示顶栏/控制条并 raise |
| `reports/fix-osc-window.md` | 本报告 |

## 4. 验证

### 4.1 构建

```bash
cd /root/github/emby-pc/app
BUILD_DIR=/tmp/t15-debug BUILD_TYPE=Debug ./scripts/build-linux.sh
BUILD_DIR=/tmp/t15-release BUILD_TYPE=Release ./scripts/build-linux.sh
```

结果：Debug/Release 均 `exit 0`；`-Wall -Wextra` 构建中 T15 改动文件无 warning（`app/src/ui/librarydata.cpp` 的 2 个 unused-function warning 为 T7 既有，不属本次范围）。

### 4.2 控制条可见性（Qt Widgets 测试）

用 `ui::PlayerPage + attachVideoSurface(dummy)` 在 offscreen 平台验证：

```text
bar true true QRect(0,511 960x89)
top true true
test_exit=0
```

说明：attach 真实视频宿主后，`PlayerControlBar` 与 `PlayerTopBar` 仍可见，且控制条占据底部 89px。

### 4.3 窗口缩放（QApplication 测试）

用 `ui::MainWindow + PlayerController` 在 offscreen 平台验证：

```text
window: minimum QSize(960, 600) -> QSize(720, 480) maximum QSize(16777215, 16777215) current QSize(752, 704) sizeGrip true
minimum QSize(720, 480) maximum QSize(16777215, 16777215) sizeGrip true
after 800x500: QSize(800, 500)
after 1200x800: QSize(1200, 800)
exportAction true helpMenus 1
test_exit=0
```

说明：最小尺寸已放宽、resize grip 开启、窗口可实际缩到 800x500 并放大到 1200x800；帮助菜单只有 1 个，诊断导出 action 存在。

### 4.4 诊断导出

offscreen 触发“导出诊断信息”后成功生成 `diagnostics-<时间>` 目录（含 diagnostics.txt），内容包含窗口 min/max/current、sizeGrip、PlayerControlBar 可见性/geometry、mpv 未初始化状态、VideoHost HWND 信息；未记录 token/Authorization。

### 4.5 启动 smoke

```bash
QT_QPA_PLATFORM=offscreen timeout 5 /tmp/t15-release/emby-pc
# exit=124（timeout 正常终止，无崩溃）
```

app.log 可见：
```text
window: minimum QSize(960, 600) -> QSize(720, 480) ... current QSize(752, 704) sizeGrip true
window resize event QSize(-1, -1) -> QSize(752, 704) minimum QSize(720, 480)
```

## 5. 未验证 / 风险

1. **无 Windows 实机与 libmpv-2.dll**：真实 `--wid` 嵌入、mpv OSC 加载、Windows 鼠标事件、D3D11VA 均无法在本容器验证；本修复以 Qt 控制条兜底，实际效果需用户用新 artifact 复测。
2. **OSC 根因未最终确认**：没有用户的 `mpv.log`。若 `EMBY_PC_MPV_OSC=1` 后仍无 OSC，app.log 的 `script-list` 与 mpv.log 可确认 builtin scripts 是否被裁剪；Qt 控制条不受影响。
3. **窗口最小尺寸在 PlayerController 里覆盖**：没有改 `app/src/ui/mainwindow.cpp`（task-15 写入范围未包含该文件）。后续可把 720x480 的最小尺寸直接落到 UI 源，避免两处不一致。
4. **VideoHost 只同步直接子窗口**：更深层窗口由 mpv 维护；若实际 mpv 版本有额外嵌套 VO 子窗口，需要看 `EMBY_PC_DEBUG_GEOMETRY=1` 的日志再调整。
5. **Qt 控制条在全屏时仍显示**：如果用户希望全屏隐藏顶栏/控制条、仅靠鼠标呼出，需要后续加自动隐藏；当前优先保证“有控制条可用”。
6. **窗口诊断日志**：正常使用时 app.log 会记录 resize（500ms 节流），日志量可接受；发布前可按需移除。

## 6. 用户复测 / 打包

请 release-engineer 用当前工作区重新构建 Windows artifact，用户按以下步骤复测：

1. 打开本地文件或 Emby 条目播放，确认底部 **Qt 控制条**可见：播放/暂停、上一集/下一集、进度条、字幕/音轨、音量、全屏、返回。
2. 拖动窗口边框/右下角 grip 缩放，确认窗口和视频区一起变化；最大化/还原正常。
3. 如仍无控制条：帮助 → **导出诊断信息…**，把生成的 `diagnostics-*` 目录（含 app.log/mpv.log）回传。
4. 如需尝试 OSC：PowerShell 设置 `$env:EMBY_PC_MPV_OSC=1` 后启动；确认 app.log 中 `options/osc=yes`、`script-list` 是否包含 osc；Qt 控制条仍会保留。
