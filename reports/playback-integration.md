# T8 播放链路接 Emby（PlaybackInfo / 直连 URL / 进度上报 / 上下集）

> 任务：task-11 · 负责人：build-verifier · 日期：2026-11-04
> 范围：`app/src/mainwindow.*`（PlayerController）、`app/src/mpvplayer.*`、`app/scripts/`（如需）、本报告。
> 依赖：T6（登录/token，task-9）、T9（日志，task-12）均已完成；未修改 `app/src/emby`、`app/src/ui` 内部实现。

## 1. 结论

- PlayerController 已从“本地文件播放”升级为 **Emby 条目播放**：`PlaybackInfo → preferredMediaSource → playbackUrl → mpv`，Authorization 通过 mpv `http-header-fields` 注入，token 不拼进 URL。
- 进度上报已实现 `Playing / Progress / Stopped`：Progress 定时 7 秒，pause/unpause、seek、切轨时立即补报，离开播放页/结束/退出时上报 Stopped。
- 上下集：详情页剧集信号 → 拉取 Episode 队列（ParentId + ParentIndexNumber/IndexNumber）→ mpv 逐集加载；`endReached` 默认自动下一集（无 10 秒倒计时，P1 未做）。
- 与 PlayerPage 对接：`track-list` JSON 解析为字幕/音轨菜单，选择时设置 mpv `sid`/`aid`；播放页 Qt 控制条在真实播放时隐藏（mpv OSC 提供控制）。
- 401/403：提示“登录已失效”并调用 `ui::MainWindow::requestLogin()`；退出登录时清理播放会话、上报 Stopped 并停止 mpv。
- 本地文件/文件夹播放保留（文件菜单），与 Emby 播放共用同一 mpv/VideoHost。

## 2. 实现

### 2.1 MpvPlayer 扩展（app/src/mpvplayer.*）

- `initialize(wid, error)` 保留为兼容重载，新增
  `initialize(quintptr wid, const QStringList &httpHeaderLines, QString *error)`：
  - 初始化前用 `http-header-fields-append` 注入 `streamHeaderLines()`；
  - 按 mpv 列表选项语法转义值中的 `\\` 与 `,`；
  - 保留 T9 的 `log-file` / `msg-level`（未覆盖）。
- 公开 `setPropertyString(const char*, QString)`、`getPropertyString(const char*)`、`command(QStringList)`；
- `loadApi()` 增加 `mpv_get_property_string` / `mpv_free` 导出符号校验。

### 2.2 PlayerController（app/src/mainwindow.*）

- 取 `ui::MainWindow::embyClient()`（T6 提供；兜底 `findChild<emby::EmbyClient*>()`），监听 `loginStateChanged`。
- UI 播放入口：
  - `ui::DetailView::playRequested / episodeActivated`（电影直播；剧集先拉 Episode 队列）；
  - `ui::HomeView::playRequested`（继续观看/最近添加）；
  - `ui::LibraryView::playRequested`（媒体库/hero；跳过 HomeView 内部的嵌套实例，避免重复请求）。
- Emby 播放流程：
  1. `fetchPlaybackInfo(itemId)`（事件先 `fetchItem` 补标题/类型/进度）；
  2. `preferredMediaSource()`；`playbackUrl(itemId, source)`（DirectPlay/DirectStream 走 `/Videos/{id}/stream?static=true&mediaSourceId=...`，否则 TranscodingUrl）；
  3. `ensurePlayer(streamHeaderLines())` → `attachVideoSurface(VideoHost)` → mpv 初始化；
  4. `setPropertyString("force-media-title", title)` → `loadPlaylist({url})`；
  5. 使用 emby `UserData.PlaybackPositionTicks` / UI progress 作为续播位置，`FILE_LOADED` 后 seek；
  6. 进入播放页、隐藏 `PlayerControlBar`、`reportPlaybackStart()`。
- 状态回填：mpv `positionChanged/durationChanged/pausedChanged/titleChanged/fileLoaded/endReached` → PlayerPage。
- 进度上报：
  - 定时器 7s（`TimeUpdate`）；
  - `pausedChanged → Pause/Unpause`、`seekRequested → Seek`、切轨 → 立即补报；
  - `backRequested`/离开播放页/`aboutToQuit`/`endReached` → `Stopped`（最终 positionTicks）。
- 上下集：
  - `loadEpisodeQueue(seriesId, ...)` 用 `ItemQuery{parentId, IncludeItemTypes=Episode, Recursive}`；
  - 队列按 season/episode 定位；上一集/下一集按钮与 `endReached` 自动下一集均调用 `playEpisodeAt()`。
- 轨道：
  - `refreshTracks()` 解析 `getPropertyString("track-list")` JSON，生成 `ui::TrackInfo`（含“关闭字幕 = no”）；
  - `sid/aid` 写入 mpv，150ms 后重新 refresh（覆盖 ui::MainWindow 的 mock 轨道回写）。
- 断线/401：`handleApiError()` 对 401/403 调用 `requestLogin()`，其它错误显示到状态栏。

## 3. 验证

### 3.1 构建

```bash
cd /root/github/emby-pc/app
./scripts/build-linux.sh          # Debug
BUILD_DIR=/tmp/emby-t8-release BUILD_TYPE=Release ./scripts/build-linux.sh
```

结果：configure/build/link 成功（exit 0）；`libemby.a`、`libui.a`、`emby-pc` 全部构建；未修改 T9 日志选项。

### 3.2 真实 Emby 服务器接口链路（C++ / EmbyClient 实测）

用容器访问 `http://<内网服务器>:8096`（测试账号，**凭据未写入仓库**；地址已脱敏），另写临时探针程序链接 `app/src/emby`：

```text
auth ok, user present: true
playSessionPresent: true sources: 1 directPlay: true directStream: true runTimeTicks: 51301760000 streams: 3
urlScheme: "http" urlPath: "/Videos/1056815/stream" tokenInUrl: false urlLen: 90
headerLines: 2 hasAuthorization: true
```

说明：样例 id=1056815；`preferredMediaSource()` 选中 DirectPlay 源；`playbackUrl()` 为 `/Videos/1056815/stream?static=true&mediaSourceId=...`，**不含 api_key/X-Emby-Token**；`streamHeaderLines()` 含 Authorization 头，符合 T8 要求。

### 3.3 集成 app

```bash
QT_QPA_PLATFORM=offscreen timeout 5 ./app/build-linux/emby-pc
# exit=124：事件循环稳定运行，被 timeout 终止，无崩溃
```

### 3.4 未能验证的部分

- 容器无 `libmpv-2.dll`、无 GPU/显示，**真实视频播放未跑通**：`ensurePlayer()` 在本机必定失败（预期）；`--wid` 嵌入、D3D11VA、HDR/DV tone-map、OSC、track-list 解析与 sid/aid 实测都只能在 Windows 实机验证。
- Emby 播放 UI 入口（Detail/Home/Library 点击）在无 mpv 的容器里无法端到端跑；已通过编译 + 真实接口探针 + offscreen 启动验证。

## 4. 改动文件

| 文件 | 改动 |
|---|---|
| `app/src/mpvplayer.h` / `app/src/mpvplayer.cpp` | headers 初始化扩展、公开 property/command、符号校验；保留 T9 日志 |
| `app/src/mainwindow.h` / `app/src/mainwindow.cpp` | PlayerController 接入 Emby：PlaybackInfo、直连 URL、进度上报、上下集、sid/aid、401/登录态处理 |
| `reports/playback-integration.md` | 本报告 |
| `app/scripts/` | 未改动（沿用 build-linux.sh / T9 的 Windows 打包脚本） |

## 5. 已知问题 / 风险

1. **mpv 未实测**：`http-header-fields-append` 的运行时行为、`track-list` 的 `get_property_string` JSON 形态、`force-media-title` 生效时机都需要 Windows 实机确认。
2. **转码回退未做运行验证**：DirectPlay/DirectStream 由 `preferredMediaSource()` 决定；若服务端直连 URL 实际 404，当前不会自动重试 TranscodingUrl（EmbyClient 已提供 `playbackUrl`，后续可加 HTTP 探测）。
3. **自动下一集无 10 秒倒计时**（P1 可取消倒计时未做）；默认开启，无设置开关。
4. **续播位置**：电影用 UI progress 估算，剧集用 Emby `UserData.PlaybackPositionTicks`；窗口打开时 seek 在 FILE_LOADED 后执行，未实机验证。
5. **UI 双重信号处理**：`ui::MainWindow` 仍连接同一批 PlayerPage/播放信号用于 mock 状态；真实状态由 mpv 事件回填纠正，轨道列表在切换后由 `refreshTracks()` 覆盖。后续应把 mock 逻辑限制在 demo/mock 模式。
6. **播放页切换即停止**：离开播放页（返回/Esc/切页）会 `mpv stop()` 并上报 Stopped；如未来要做“后台继续播放”，需要调整。
7. **日志/token**：`app.log` 不记录 token（T9 约束）；mpv `http-header-fields` 日志级别 all=v 下**可能包含 Authorization 头**，Windows 验收回传 mpv.log 前需检查（见 T9 验收清单）。

## 6. Windows 验收建议

1. 用 T9 的 `scripts/package-windows.ps1` 出包，放入 `libmpv-2.dll` 后启动。
2. 登录测试服务器（`http://<内网服务器>:8096`，测试账号），进入媒体库点击样例电影 **1056815** 播放：确认开始播放、无 api_key URL、mpv stats 中 `hwdec-current=d3d11va`。
3. 拖动进度/暂停/切字幕/切音轨/上一集/下一集；确认 Emby 服务器“活动会话”中进度及时更新（7 秒内）。
4. 剧集连续播放到一集结束，确认自动下一集并重新上报 Playing。
5. 播放中返回详情页/按 Esc，确认停止播放并上报 Stopped（活动会话消失）。
6. 若出现 401，确认弹出重新登录且 token 不在 URL/日志中。

## T48 实体视频无法播放诊断与直连兜底

> 任务：task-48 · 负责人：build-verifier · 日期：2026-11-06

### 1. DirectPlay 优先服务器 DirectStreamUrl

实测实体库 PlaybackInfo 返回：
`SupportsDirectPlay=true, DirectStreamUrl=/videos/{id}/original.mp4?DeviceId=...&MediaSourceId=...&PlaySessionId=...`；
旧逻辑无条件自拼 `/Videos/{id}/stream?static=true`。现改为：
1) `supportsDirectPlay && DirectStreamUrl 非空` → `resolveMediaUrl()` 解析；
2) 保留 DeviceId/MediaSourceId/PlaySessionId，去掉 api_key/X-Emby-Token；
3) 为空才回退自拼 static URL；
4) 候选顺序：playbackUrl → 自拼 directStream → 原始 DirectStreamUrl → TranscodingUrl；
5) 首次 8 秒无 file-loaded / mpv error 自动试下一个，全部失败弹一次性提示。

验证（无需 mpv）：
```text
PASS server DirectStreamUrl path preferred
PASS DeviceId preserved
PASS MediaSourceId preserved
PASS PlaySessionId preserved
PASS api_key stripped
PASS self static fallback
PASS transcoding URL resolved/token stripped
failures 0
```

### 2. PlaybackInfo 脱敏日志

- 每个播放记录：method、supportsDirectPlay/Stream/Transcoding、mediaSourceId、
  hasDirectStreamUrl/TranscodingUrl、最终 URL scheme+host+path（不含 query）、
  runTimeTicks、container、video codec/尺寸、audio codec/channels、candidates 数量。
- 每个候选记录序号与脱敏 URL。

### 3. 失败诊断

- mpv 加载后启动 8 秒 watchdog；`file-loaded` 停止。
- 超时或 `errorOccurred`/`MPV_EVENT_END_FILE` error：记录 item、mediaSourceId、method、
  candidate、脱敏 URL、mpv `error`/`http-status`。
- 全部候选失败：`showStatus` + 非模态一次性 QMessageBox：「播放失败，已记录诊断，请导出 app.log/mpv.log」；
  `EMBY_PC_TEST_NO_DIALOG=1` 可在自动测试中跳过弹窗。
- mpv 日志：`msg-level=all=v` 保留 HTTP 细节；`MPV_EVENT_LOG_MESSAGE` 的 error/fatal 文本经
  `redactSensitiveLog()` 去掉 Authorization/api_key/Token 后再写 app.log。

### 4. 待实机

- 实体库 1050415：日志应出现 server DirectStreamUrl 候选、file-loaded；若失败自动 retry 自拼 URL，
  最终日志包含 mpv error/http-status。
- 失败时一次性提示，导出 app.log/mpv.log；日志中无 token/Authorization。
