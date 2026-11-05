# T28 设置页补全 + 关于页（M3-2 / SET-01、SET-03）

> 任务：task-28（T30 最小尺寸小修一并合入） · 日期：2026-11-05
> 范围：app/src/ui/settingspage.*、mainwindow.*、style/dark.qss、demo_main.cpp、app/src/mainwindow.cpp（仅 T15 最小尺寸覆盖）、README / window-frame 报告。

## 1. 设置页结构

沿用深色样式，页面可滚动，分为：

1. **服务器与账号**：连接状态、服务器 URL、用户名、服务器版本、Token 存储后端；登录/重新登录、退出登录。
2. **播放**：
   - 默认字幕语言：简中优先 / 繁中优先 / 英文优先 / 不自动开启；
   - 自动连播开关；
   - 音量记忆开关（关闭后不恢复上次音量）；
   - 控制条自动隐藏延时：1.5s / 2.5s / 3s；
   - 硬件解码只读文本（由 PlayerController/T29 回填；未初始化显示“未初始化”）；
   - 视频渲染/HDR 只读说明。
3. **存储与日志**：海报缓存占用（递归读取 `CacheLocation/images`）、清理按钮、缓存上限（128MB 步进，64–8192MB）、打开日志目录、导出诊断信息。
4. **关于**：应用版本、Qt 版本、构建日期（`__DATE__ __TIME__`）、libmpv 版本（T29 回填）、复制诊断信息（不含 token）。
5. **界面/数据源**：主题、语言、海报尺寸只读；Mock/真实数据开关保留。

## 2. QSettings 键约定（T29 读取）

| Key | 类型 | 默认 | 说明 |
|---|---|---|---|
| `player/subtitleLanguage` | string | `chi,zho,zh` | mpv slang 顺序；可选 `chi,zho,zh` / `zho,chi,zh` / `en` / `none`（不自动开启） |
| `player/autoPlayNext` | bool | `true` | 自动连播 |
| `player/rememberVolume` | bool | `true` | 是否恢复上次音量 |
| `player/volume` | int | `80` | 上次音量 0–100（MainWindow 在音量变化且 rememberVolume=true 时写入） |
| `player/autoHideDelayMs` | int | `2500` | 控制条自动隐藏延时 |
| `cache/imageLimitMB` | int | `1024` | 海报磁盘缓存上限，MainWindow 应用到 `EmbyThumbnailCache::setDiskCacheMaxBytes` |

`none` 是“不自动开启”的显式值，避免空字符串在 QComboBox data 中无法查找。

## 3. 对外 API

```cpp
QString subtitleLanguage() const;
bool autoPlayNext() const;
bool rememberVolume() const;
int autoHideDelayMs() const;
int cacheImageLimitMB() const;
void setHardwareDecoderText(const QString &text);
void setLibmpvVersionText(const QString &text);
void refreshCacheUsage();
void setSession(...); void clearSession();
// signals
void settingsChanged();
void clearImageCacheRequested();
void exportDiagnosticsRequested();
void copyDiagnosticsRequested();
```

MainWindow 额外暴露 `SettingsPage *settingsPage() const`，T29 可在播放初始化/状态变化时回填硬件解码与 libmpv 版本。

## 4. MainWindow 集成

- 构造时读取 QSettings：应用控制条延时、缓存上限；rememberVolume=true 时恢复音量。
- `settingsChanged` 实时应用上述设置。
- 清理缓存：`EmbyThumbnailCache::clear()` + 删除 `CacheLocation/images` 文件 + 刷新占用。
- 导出诊断：复用 T17 的 `ExportDiagnosticsAction`（帮助菜单），找不到时状态栏提示。
- 复制诊断：设置页直接写剪贴板（版本/Qt/构建/libmpv/缓存/播放设置，无 token/服务器凭据）。
- 打开日志目录：`QDesktopServices::openUrl(AppLocalDataLocation)`。

## 5. T30 最小尺寸对齐

- `ui::MainWindow` 最小尺寸改为 **800x480**（构造与 WindowFrame 标题栏初始化两处）。
- 删除 PlayerController 里 T15 的 `setMinimumSize(720,480)` 覆盖；保留 size grip 与小屏初始尺寸收紧逻辑（下限用 800x480）。
- T26 `WM_GETMINMAXINFO` 取 `max(window.minimumSize(), 800x480)`，因此 Win32 与 widget 层一致。
- 播放页 `VideoHostPlaceholder` 仍为 320x180 下限；播放器模式全窗口由 mpv 绘制，不额外加高下限。

## 6. 自测

`QT_QPA_PLATFORM=offscreen emby-ui-demo --selftest`：**151 项 ALL PASS**（本地假 Emby 服务器/offscreen，不访问真实服务器）。

T28 新增覆盖：

- 设置页控件存在/默认值；
- 修改后发出 `settingsChanged`；
- QSettings 六个键写入；
- 清理缓存/导出诊断按钮发出信号；
- 硬件解码/libmpv 回填、关于字段非空、缓存占用显示；
- 重新构造 `SettingsPage` 恢复值。

T30 新增：`minimumSize==800x480`，且 `resize(700,400)` 后尺寸仍不小于 800x480；视频宿主 placeholder 下限仍为 320x180。

## 7. 已知限制

- 硬件解码与 libmpv 版本由 T29/PlayerController 在播放初始化后回填；未播放时显示“未初始化”。
- “复制诊断信息”是设置页内生成的 UI 摘要，完整 app.log/播放诊断仍走 T17 的“导出诊断信息”。
- Windows 实机需再确认 DPI/小屏下 800x480 布局与设置页滚动；视频 320x180 下限由 placeholder 保证。

## 8. T29 设置生效 + 播放会话收尾（Controller）

> 任务：task-29 · 负责人：build-verifier · 日期：2026-11-05
> 范围：app/src/mainwindow.*、app/src/mpvplayer.*（SettingsPage 不改，T28 提供访问器/信号）。

### 8.1 启动读取与实时应用

- PlayerController 构造时读取 QSettings：
  - `player/rememberVolume`、`player/volume`：remember=true 时把音量恢复到最后音量（PlayerPage slider；mpv 初始化后 `applyMpvSettings()` 再次同步到 mpv）；
  - `player/autoPlayNext`：更新 `m_autoNextEpisode`，endReached 连播逻辑使用该值；
  - `player/subtitleLanguage`：保存为 `m_subtitleLanguage`，`none`/空值规范化为 mpv `slang=no`；
  - `player/autoHideDelayMs`：应用 `PlayerPage::setOverlayHideDelay()`（500–10000ms）。
- 连接 `SettingsPage::settingsChanged` → `PlayerController::applyPlayerSettings()`：实时更新 slang、自动连播、音量记忆/音量、控制条延时；若 mpv 尚未初始化，参数留在成员，`ensurePlayer()` 成功后 `applyMpvSettings()` 补应用。
- 音量变化仍由 UI 的 `PlayerPage::volumeChanged` 写回 `player/volume`（remember=true 时），不改变 T28 行为。

### 8.2 硬件解码 / libmpv 版本

- `MpvPlayer` 新增信号：`hardwareDecoderChanged(hwdecCurrent, vo, gpuApi)`、`libmpvVersionChanged(version)`。
- `MpvPlayer::initialize()` 成功后读取 `hwdec-current`/`vo`/`gpu-api`/`mpv-version` 并 emit；同时 observe `hwdec-current`，片源切换/硬解变化时再次 emit。
- `PlayerController` 将信号转发到 `SettingsPage::setHardwareDecoderText()` / `setLibmpvVersionText()`；设置页/关于页只读显示；未初始化时保持“未初始化”。
- `fileLoaded` 和 `ensurePlayer()` 额外调用 `updateHardwareInfo()` 兜底，保证播放开始后文本正确。

### 8.3 关闭上报 Stopped（SYN-03）

- `PlayerController` 在 `QEvent::Close`（窗口 closeEvent 路径）调用 `reportPlaybackStopped()`；
- 析构函数和既有 `QCoreApplication::aboutToQuit` 路径兜底，`reportPlaybackStopped()` 内部 `m_reporting` 防重复；
- 位置使用 `m_lastKnownPositionSeconds`（无效时才用 `m_player->position()`），不会上报 0；
- 先上报 Stopped 再退出，首页刷新逻辑保留（Stopped 回调后 `scheduleHomeRefresh(0)`）。

### 8.4 进度间隔

- `m_progressTimer` 由 7000ms 改为 **10000ms**（规格 10 秒）；
- Pause/Unpause、Seek、SubtitleTrackChange、AudioTrackChange 仍按原逻辑立即上报，不依赖定时器。

### 8.5 验证

- Linux app Debug 构建通过；`-Wall -Wextra` 构建无新增 warning。
- T28 `emby-ui-demo --selftest` 151 项基线通过（设置页/默认值/信号/QSettings）。
- T29 的设备相关路径（真实 mpv hwdec/libmpv、真实 Emby Stopped）需 Windows 实机/播放验收；不访问真实服务器做自动测试。
- 验收点：改设置后立即生效；播放时设置页硬件解码/关于页 libmpv 版本有值；关闭窗口 app.log 出现 `playback stopped report` 且 positionSeconds>0；10 秒一次 TimeUpdate。
