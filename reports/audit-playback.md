# 播放兼容性全面审查（T61）

> 任务：task-61「全面审查#4：播放兼容/硬解/HDR/字幕/音轨/本地文件」；**只审查，不改代码**。
> 范围：`app/src/mpvplayer.*`（mpv 选项/事件/命令）、`app/src/videohost.*`（--wid 宿主/鼠标键盘转发）、`app/src/mainwindow.*`（PlayerController：播放/轨道/进度/续播/错误）、`app/src/emby/*`（PlaybackInfo/URL/DeviceProfile/进度上报）、`app/src/ui/playerpage.*`（轨道菜单/控制条）、`app/src/ui/settingspage.*`（设置项）。
> 基线：当前工作区代码、`docs/requirements.md` v0.1、`docs/windows-acceptance.md`、mpv 官方手册/源码、真实 Emby 联调报告（`reports/emby-live-validation.md`）。静态审查；容器无 Windows/GPU，**未运行视频播放**。
> 日期：2026-11-06

## 0. 结论摘要

- **确认 P0：6 个**（代码可定位，会阻塞 0.1.0 验收或明显违背 P0 验收条目）。
- **P0 口径待决：1 个**（验收文档要求 mpv OSC，当前实现默认关闭 OSC、保留 Qt 控制条；若按现文档执行，O-01~O-08 会全部 FAIL）。
- **P0 验证风险：2 个**（HDR10 tone-map、DV P5/P8 无法在 Linux 容器验证；配置合理但需 Windows S1~S3 实机确认，失败则升为 P0）。
- **P1：12 个；P2：18 个**（见 §3/§4）。

| # | P0 问题 | 一句话影响 | 预估 |
|---|---|---|---|
| P0-1 | Emby 播放时 **N/P 键不能切上下集** | `n/p` 被绑定到 mpv `playlist-next/prev`，而 Emby 播放列表只有当前 1 个 URL，按键无任何效果 | 0.25d |
| P0-2 | **双击视频不能进入应用全屏** | 未覆盖 mpv 内置 `MBTN_LEFT_DBL cycle fullscreen`，双击操作的是 mpv 子窗口而非 Qt 窗口，全屏状态/UI 会错乱或无效 | 0.25d |
| P0-3 | **续播缺少 >95% 重置与“自动续播”开关** | PLY-10 两条验收（>95% 从头、设置关闭后从头）均未实现 | 0.5d |
| P0-4 | **播放错误处理不达标** | 只有通用弹窗+“确定”，无错误码/重试/返回；候选 URL 最多 4 个（3 次重试）超过“最多 2 次”；本地文件错误只在状态栏 5s | 1d |
| P0-5 | **正常退出丢失最后一次 Stopped/进度** | 退出时异步 POST 后事件循环立即结束，`EmbyClient` 析构又 abort 在途请求；服务器最后进度可能不更新 | 0.5d |
| P0-6 | **硬解失败无 auto-safe 降级与提示** | 强制 `hwdec=d3d11va`，mpv 静默软解；没有自动降级链、没有用户提示，NFR-04 不达标 | 0.5d |

**批修建议顺序**：P0-1/2（0.5d）→ P0-3/6（1d）→ P0-4（1d）→ P0-5（0.5d）→ 再批量清 P1（§3）。

## 1. 审查范围与方法

- 代码：`app/src/mpvplayer.cpp`(518 行)、`app/src/videohost.cpp`(392)、`app/src/mainwindow.cpp`(1793)、`app/src/emby/*`、`app/src/ui/playerpage.cpp`(1192)、`app/src/ui/settingspage.cpp`(790)。
- 验收标准：`docs/requirements.md` 的 PLY-01~13、SYN-01~03、NFR-01~05、SET-01；`docs/windows-acceptance.md` 的 O/P/S/W 组。
- 参考：`reports/emby-live-validation.md`（真实 Emby 4.9.1.90，直连 206/Authorization 头已验证）、`reports/playback-integration.md`（已知风险，其 §5.7 已提示日志 token 风险）。
- 限制：Windows 实机/GPU/libmpv.dll 均不可用；`--wid` 嵌入、D3D11VA、HDR/DV、OSC、ASS 实际渲染均需按 §6 在 Windows 验证。本文所有“现象”均由代码推导，并标注了触发条件。

## 2. P0 问题（阻塞发布）

### P0-1 Emby 播放时 N/P 键不能切上下集（PLY-08/PLY-09）

**证据**

- 自定义输入绑定只把 `n/p` 绑到 mpv 播放列表：`app/src/mpvplayer.cpp:75-89`（`n playlist-next`、`p playlist-prev`）。
- Emby 播放时每次只加载**一个** URL：`app/src/mainwindow.cpp:1195-1208`（`loadPlaylist({m_currentPlaybackUrl}, 0)`）。
- 上下集逻辑只从 Qt 控制条的“上一集/下一集”按钮进入：`app/src/mainwindow.cpp:330-349`（`previous/nextEpisodeRequested` → `stepEmbyEpisode`）。**没有**任何 `script-message` 把 mpv 的 n/p 事件转给 PlayerController。
- 因此 Emby 条目播放时 mpv 播放列表只有 1 项，`playlist-next/prev` 是空操作；Qt 按钮又不受键盘触发。

**现象/触发**：任意剧集播放中按 `n` 或 `p` → 无反应；只能在 Qt 控制条点击“下一集”。与 PLY-08「N/P 上下集」、PLY-09「N/P 切换 <1 秒」直接冲突。

**修复建议**

1. `kInputConf` 改为：`n script-message app-next-episode`、`p script-message app-prev-episode`（保留 mpv 列表场景的兜底）。
2. `MpvPlayer` 处理这两个 client-message，发信号给 PlayerController；PlayerController 复用 `stepEmbyEpisode(±1)`；本地播放回退 `playlistNext/Previous()`。
3. 队列未建好时走现有的 `requestEpisodeSiblings()` 异步补队列，避免第一下无反应。

**预估**：0.25d；风险低（与现有按钮路径同一条）。

### P0-2 双击视频不能进入应用全屏（PLY-08/PLY-13）

**证据**

- `VideoHost::mouseDoubleClickEvent` 把双击转发给 mpv：`app/src/videohost.cpp:198-231`；`PlayerController` 连接处补 `double`：`app/src/mainwindow.cpp:176-185`。
- 自定义 `kInputConf` **没有**覆盖 `MBTN_LEFT_DBL`：`app/src/mpvplayer.cpp:75-89`。
- mpv 默认绑定内置在二进制里（`etc/input.conf` 编译时去掉注释）：`#MBTN_LEFT_DBL cycle fullscreen` 会成为默认绑定（mpv 源码 `etc/input.conf` 注释：“On compilation, this file is baked into the mpv binary, and all lines are uncommented”）。它操作的是 **mpv 自己的窗口**（这里是 `--wid` 子窗口），不是 Qt 顶层窗口。
- Qt 侧没有双击全屏逻辑：`app/src/ui/playerpage.cpp` 的 `eventFilter` 只在 `MouseButtonDblClick` 时 `resetWindowDrag()`（`:696-700`），没有 `fullscreenRequested`。

**现象/触发**：播放中双击画面 → 要么无反应，要么 mpv 对子窗口执行 `cycle fullscreen`（窗口样式/尺寸脱离 Qt 管理，HUD/控制条位置错乱）；不会进入 `ui::MainWindow::toggleFullscreen()`。与 PLY-08「双击视频全屏」、PLY-13 冲突。

**修复建议**

1. `kInputConf` 增加：`MBTN_LEFT_DBL script-message app-toggle-fullscreen`（覆盖默认绑定，进入应用全屏路径）。
2. 保持现有 `mouse ... double` 转发（mpv 的 `mouse` 命令第 4 参数就是 `<single|double>`，见 mpv `DOCS/man/input.rst`，当前传值正确）。
3. 回归：双击进入/退出全屏后，再次双击仍能退出；全屏下 Qt 控制条可呼出。

**预估**：0.25d。

### P0-3 续播缺少 >95% 重置与“自动续播”开关（PLY-10）

**证据**

- 电影入口用 UI progress 估算续播点：`app/src/mainwindow.cpp:783-788`（`progress × runtimeMinutes`，progress 为 0~1）。
- 继续观看/媒体库入口用 `userData.playbackPositionTicks`：`app/src/mainwindow.cpp:812-813`；队列条目同样保存续播点：`app/src/mainwindow.cpp:855`、`:926`、`:1025`。
- `handlePlaybackInfo` 无条件使用 `startPositionTicks` 作为 `m_pendingSeekSeconds`：`app/src/mainwindow.cpp:1165`；`fileLoaded` 后 seek：`:246-255`。
- **没有** >95% 判断（全仓库 grep `0.95`/`95%` 无相关逻辑）。
- **没有**“自动续播”设置项：`applyPlayerSettings` 只读 `player/autoPlayNext`、`player/rememberVolume`、`player/subtitleLanguage`、`player/autoHideDelayMs`（`app/src/mainwindow.cpp:486-509`）；设置页也只有字幕语言/自动连播/音量记忆/自动隐藏（`app/src/ui/settingspage.cpp:193-215, 417-447`）。

**现象/触发**：① 已看 98% 的条目，点播放仍从 98% 开始（应从头）；② 用户想关闭续播，只能手动拖回 0；PLY-10 两条验收均失败。

**修复建议**

1. 新增 `player/autoResume`（默认 true）并在设置页加开关。
2. 统一入口函数 `resumeTicksFor(item, duration)`：关闭续播 → 0；`position >= 0.95 * duration` → 0；否则原值。
3. 对电影/继续观看/队列三个来源统一调用；自动下一集默认 0（或同样规则），避免 `entry.startPositionTicks` 的旧进度导致下一集莫名续播。
4. 设置变更即时生效（沿用 `settingsChanged` → `applyPlayerSettings`）。

**预估**：0.5d。

### P0-4 播放错误处理不达标（PLY-12）

**证据**

- 失败入口只收集 reason 并弹通用框：`app/src/mainwindow.cpp:1236-1267`（固定文案“播放失败，已记录诊断…”，仅 `QMessageBox::Ok`，无重试/返回、无错误码）。
- 候选 URL 依次尝试到失败：`app/src/mainwindow.cpp:1210-1224`；候选最多 4 个（`playbackUrl`、自拼 static、服务器 DirectStreamUrl、TranscodingUrl，见 `:1098-1112`），即最多 3 次重试，超过 PLY-12“最多 2 次”。
- 本地文件错误只走状态栏：`app/src/mainwindow.cpp:286-297`（`errorOccurred` 且非 Emby → `showStatus(message, 5000)`），没有 3 秒内可读弹窗/重试。
- 403/404/超时/解码失败无分类映射；`http-status` 只在失败日志里读一次：`:1253`。

**现象/触发**：断网/服务器 403/文件被删除/解码失败 → 用户只看到“播放失败，已记录诊断”或状态栏一行；无法重试；连续 4 个候选各等 8s（见 P1-3）后才失败。

**修复建议**

1. 建 `PlaybackError` 映射：`http-status` 403/404/500、`mpv error`（加载/解码）、网络超时；文案含“错误码 + 建议”。
2. 弹窗按钮：`重试`（重新走 PlaybackInfo/候选）、`返回`（退出播放页）；重试计数器 ≤2。
3. 本地文件错误也弹同一错误框（路径 + 原因）。
4. 失败时停止会话（见 P1-5），避免服务器幽灵会话。

**预估**：1d。

### P0-5 正常退出丢失最后一次 Stopped/进度（SYN-03）

**证据**

- 退出路径：`QCoreApplication::aboutToQuit` → `reportPlaybackStopped()`（`app/src/mainwindow.cpp:438-443`）；`QEvent::Close` 事件过滤器同样（`:545-549`）；析构兜底（`:478-484`）。
- `reportPlaybackStopped` 只发异步 POST，立即把 `m_reporting` 置 false：`app/src/mainwindow.cpp:1724-1754`。
- `main.cpp` 在 `app.exec()` 返回后直接 `CrashHandler::shutdown()` / 关闭日志：`app/src/main.cpp:210-216`，**没有**再跑事件循环等待网络完成。
- `EmbyClient::~EmbyClient` 会遍历 `m_inFlight` 并 `abort()`：`app/src/emby/embyclient.cpp:105-113`。

**推导**：正常关闭时，Stopped POST 刚创建就被析构 abort（或根本没有 socket 事件机会），服务器保存的续播位置可能停在 10 秒前（上一次 TimeUpdate），违反 SYN-03“正常关闭应用不丢最后一次进度”。

**修复建议**

1. 退出前同步等待：`reportPlaybackStopped()` 返回一个可等待对象；在 `closeEvent`/`aboutToQuit` 中用 `QEventLoop` + `QTimer::singleShot(1500ms)` 等待回调（超时放行），期间不销毁 `EmbyClient`。
2. 或给 Stopped 请求单独的 `QNetworkAccessManager`/同步 socket，并在 `EmbyClient` 析构前 `waitForFinished`（GUI 退出可接受 1s 阻塞）。
3. 至少保证 `QSettings::sync()` 与最后的 `m_lastKnownPositionSeconds` 落盘，下次启动可补报（次级方案）。

**预估**：0.5d；需 Windows 实测“播放 15 秒→关闭→服务器 Continue Watching 位置”。

### P0-6 硬解失败无 auto-safe 降级与提示（NFR-04）

**证据**

- Windows 强制硬解：`app/src/mpvplayer.cpp:219-223`（`gpu-api=d3d11`、`gpu-context=d3d11`、`hwdec=d3d11va`）。
- **没有** `auto-safe` 回退链：全仓库 grep `auto-safe` 0 命中；初始化失败只弹“播放内核初始化失败”（`app/src/mainwindow.cpp:749-757`）。
- 播放中 `hwdec-current` 只在设置页只读显示（`:529-541`），失败/软解时播放页无任何提示。
- mpv 手册：`--hwdec=<api>` 在硬解不可用时**会回退软件解码**（`/tmp/options.rst:1177-1180`），所以不会崩溃，但用户会看到高 CPU/掉帧而不知道原因；NFR-04 要求“自动降级 auto-safe 并提示”。

**修复建议**

1. 初始化/文件加载后检查 `hwdec-current`；若为 `no` 或含 `-copy`（或空），在播放页状态栏/OSD 给一次提示“硬件解码不可用，已回退软解（可能卡顿）”。
2. 提供降级链：`d3d11va` 失败 → `auto-safe`（mpv 会自动挑可用的安全硬解）→ 软解；可通过重新初始化或启动时按 `hwdec-current` 判断后 `setProperty("hwdec","auto-safe")` 实现（运行时切换需验证）。
3. 设置页增加“硬解模式：自动（推荐）/ d3d11va / 关闭”三档，默认自动（内部仍优先 d3d11va）。
4. 设置页同时展示最后一次实际生效值（已有 `hwdec-current` 文本，可保留）。

**预估**：0.5d。

## 3. P1 问题（首个稳定版前修，MVP 后可迭代）

| # | 问题 | 证据 | 现象/触发 | 修复建议 | 预估 |
|---|---|---|---|---|---|
| P1-1 | **键盘可能双转发**（空格/F/I 触发两次） | 同时安装 app 与 window 两层过滤器：`mainwindow.cpp:438-453`；转发逻辑不区分来源：`:554-560`；`isDuplicateKeyForward(name, 0)` 两个来源都传 0：`:1473-1495`。若按键事件的接收者是 m_window，两层过滤器都会调用转发 | 点击视频后按空格：若 pause 被切换两次则表现为“没反应”；F/I/N/P 同理。需 Windows 5 分钟实测 | 只保留 app 级过滤器；或按 `watched`/事件指针去重，同一 QKeyEvent 只转发一次 | 0.25d |
| P1-2 | **生产日志 all=v / mpv.log 未脱敏、未轮转** | `mpvplayer.cpp:286-289`（`log-file` + `msg-level=all=v` + 512MiB cache）；app.log 有脱敏（`:489-499`），但 mpv 自己写的 mpv.log 不经过该函数；`reports/playback-integration.md:109` 已提示 Authorization 风险 | 4K 长播产生大日志、CPU I/O；若 mpv 在 verbose 下记录 header，则 token 进 mpv.log（NFR-05 风险） | 发行版默认 `msg-level=info`；DEBUG 由设置页开关；给 mpv.log 加大小上限/轮转；播出后 grep 确认无 Authorization | 0.5d |
| P1-3 | **8 秒 watchdog 对慢起播误判** | `mainwindow.cpp:199-203`（8s）、`:1206-1207`、`:1226-1234` | 4K 远程/服务端转码 HLS 起播 >8s 时被判失败，转去尝试更差的候选 URL，最终弹“播放失败” | 首次起播 25~30s；收到 `file-loaded` 或首个视频/缓存事件即停；区分“无响应”和“正在加载” | 0.25d |
| P1-4 | **任意 mpv error 日志都会触发候选切换** | `mpvplayer.cpp:489-499` 把 error/fatal 日志一律 emit errorOccurred；`mainwindow.cpp:291-294` 在未 file-loaded 时 `tryNextPlaybackCandidate` | 非致命错误（外挂字幕加载失败、字体缺失、硬解告警）会导致放弃当前直连 URL、回退到转码/静态 URL，浪费 8s | 只有 `MPV_EVENT_END_FILE` 的 error 或 http-status 4xx/5xx 才切候选；log error 仅记录 | 0.5d |
| P1-5 | **PlaybackStart 早于 fileLoaded；失败不 Stopped** | `mainwindow.cpp:1179`（先 reportStart）与 `:246-262`（fileLoaded）；失败路径 `:1236-1267` 无 `reportPlaybackStopped` | 起播失败后服务器留下“正在播放”的幽灵会话；进度定时器继续发送位置 0 | fileLoaded 后再 PlaybackStart（或 start 时带正确续播位置）；失败/退页必发 Stopped | 0.5d |
| P1-6 | **音量记忆不生效** | `settingspage.cpp:442-443` 只写默认 80；`mainwindow.cpp:490-492` 只读；`volumeChanged` 只改 mpv（`:357-359`），全仓库没有保存 `player/volume` | 用户把音量调到 30，重启回到 80；“音量记忆”开关无意义；滚轮/↑↓ 改的音量不落盘 | `volumeChanged` 防抖写 QSettings（仅 rememberVolume=true 时）；退出/关闭时保存 | 0.25d |
| P1-7 | **多显示器全屏不在鼠标所在屏** | `ui/mainwindow.cpp:1452` 直接 `showFullScreen()`；没有 `screenAt(QCursor::pos())`/`setScreen` | 窗口在 A 屏、鼠标在 B 屏，按 F 全屏仍留在 A 屏；PLY-13 要求鼠标所在屏 | 全屏前 `setScreen(QGuiApplication::screenAt(QCursor::pos()))` 并夹取 geometry；恢复时记住原屏 | 0.25d |
| P1-8 | **Emby 外挂字幕不加载** | `embytypes.h:98-127` 解析了 `DeliveryUrl/IsExternal/IsTextSubtitleStream`，但全仓库无使用；`refreshTracks` 只读 mpv `track-list`（`mainwindow.cpp:1382-1426`） | 媒体库大量外挂 .ass/.srt 的条目，播放时字幕菜单只有内嵌轨/“关闭字幕”，看不到外挂字幕 | 播放前从 `MediaStreams` 过滤 `Subtitle && IsExternal`，用 `resolveMediaUrl(deliveryUrl)` 生成 URL，通过 mpv `sub-add` + Authorization 头加载 | 1d |
| P1-9 | **音频直通缺失** | 全仓库无 `audio-spdif`/`audio-device` 设置；仅默认 decoded PCM；P1-04 列为 P1 功能 | 家庭影院用户无法把 AC3/EAC3/DTS/TrueHD 直通功放；Atmos 无法点亮 | 设置页加“音频直通（HDMI）”开关，映射 `audio-spdif=ac3,eac3,dts,truehd,dts-hd` + `audio-channels=7.1,5.1,stereo`；失败回退 PCM 并提示 | 0.5d |
| P1-10 | **DV 未检测/无降级提示** | `embytypes.h:102-107` 解析 `VideoRange/ExtendedVideoType`，全仓库无使用；配置固定 SDR tone-map（`mpvplayer.cpp:224-230`） | DV P5 若 libmpv 构建缺 libdovi → 绿屏/紫屏，用户无提示；DV P7 情况未知；PLY-04 P0 验收风险 | 播放前读 `source->videoStream()->extendedVideoType`；若含 DolbyVision，在日志/状态栏标注；失败时提示“当前片源 DV，若颜色异常可切换转码” | 0.5d |
| P1-11 | **track-list 无观察，转码/切轨后不刷新** | 仅在 fileLoaded+150/700ms 拉一次 `track-list`：`mainwindow.cpp:259-261, 1382-1426`；没有 `mpv_observe_property("track-list")` | HLS 转码时轨道可能晚于 700ms 出现；外挂字幕 `sub-add` 后菜单不更新 | 观察 `track-list`/`sid`/`aid` 属性变化，变化时 refreshTracks（去抖 100ms）；同时观察 `playlist-pos` 更新本地播放标题/上下集状态 | 0.25d |
| P1-12 | **验收口径冲突：OSC vs Qt 控制条** | 代码默认 `osc=no`（`mpvplayer.cpp:242-243`，仅 `EMBY_PC_MPV_OSC=1` 开启），并保留 Qt 控制条（`ui/playerpage.cpp:980-1005`）；`docs/windows-acceptance.md` O-01~O-08、`requirements` PLY-07 写的是 mpv OSC；`reports/playback-integration.md:12` 又写“播放时隐藏 Qt 控制条、由 OSC 提供控制”（已过时） | 按现验收文档执行：鼠标移入看不到 OSC，O-01~O-08 全 FAIL；但功能上 Qt 控制条存在 | 由 lead 冻结口径：**推荐保留 Qt 控制条**并更新 requirements/acceptance（OSC 仅作为 `EMBY_PC_MPV_OSC=1` 的调试选项）；若必须 OSC，则改默认 `osc=yes` 并去掉 Qt 条（工作量大，且与 T15 决策冲突） | 0.25d（改文档）/ 1d（改实现） |

补充：**P1-13 候选 URL 顺序与 HTTP 探测**——`playbackUrl()` 已优先服务器 DirectStreamUrl；失败后还会尝试自拼 `/stream?static=true`（`mainwindow.cpp:1109-1112`），即使服务端判定不能 direct play。该兜底在实体/STRM 库有价值（T48），但应先用轻量 HTTP HEAD/首字节探测再交给 mpv，否则每个坏候选浪费一个 8s 超时（与 P1-3 叠加）。预估 0.5d，可并入 P0-4 一起做。

## 4. P2 问题（可排期，不影响 MVP 主路径）

| # | 问题 | 证据 | 说明/建议 |
|---|---|---|---|
| P2-1 | 鼠标中键/右键编号疑似互换 | `videohost.cpp:171-184`：左=0、中=1、右=2；mpv 约定 0=左、1=右、2=中（`MOUSE_BTN3` 起为 WHEEL_UP，见 mpv input.rst） | 若成立：右键触发中键、中键触发右键；`MBTN_MID` 探针绑定永远不触发。用 OSC 右键菜单或 `--input-test` 验证后改映射 |
| P2-2 | 滚轮/↑↓ 改音量后 Qt 滑条不同步 | mpv 默认 `WHEEL_UP add volume 2`（内置 input.conf）作用于 mpv；Qt 只监听自己的 slider（`ui/playerpage.cpp:417`），未观察 mpv `volume` | 观察 `volume` 属性回填滑条；或控制器在 `addVolume` 后 setVolume 滑条 |
| P2-3 | 截图无 UI、默认存 CWD | 全仓库无 screenshot 代码；mpv 内置 `s screenshot` 会存到工作目录（安装到 Program Files 时不可写 → 静默失败） | P1-06 功能；设置 `screenshot-directory` 到图片目录 + 状态提示；或加“截图”按钮 |
| P2-4 | 本地文件夹：非递归、排序可能不自然 | `mainwindow.cpp:52-68`（`QDir::Name`、仅当层、扩展名白名单） | 递归可选；按 `QCollator` 自然排序；补 `.m2ts/.mts/.flv` 等扩展名 |
| P2-5 | 本地播放列表标题卡在第一集、上下集按钮状态不更新 | `force-media-title` 只设一次：`mainwindow.cpp:709`；`setEpisodeNavigationEnabled` 只在 playFiles 设一次：`:704-708`；无 `playlist-pos` 观察 | 切文件时更新标题（media-title 被 force 覆盖）；观察 `playlist-pos` 更新按钮/集号 |
| P2-6 | 多版本 MediaSource 无选择 | `embytypes.cpp:517-524` 取第一个 DirectPlay 源 | 多版本（4K/1080p/导剪版）无法选；后续加 PlaybackInfo 源选择菜单 |
| P2-7 | `RequiredHttpHeaders` 未使用 | `embytypes.h:160` 解析；无使用点 | 部分 STRM/IPTV 源需要每源自定义头；直连会 401/403。合并到 P1-8 的 URL/头处理 |
| P2-8 | `CanSeek` 固定 true | `embytypes.h:328` 默认 true，`toJson()` 直接写 | VOD 正常；未来直播/不可 seek 源会误导服务器 |
| P2-9 | PlaybackStart 的 PositionTicks 为 0 | `mainwindow.cpp:1685`（`m_player->position()` 此时通常 0） | 用 `m_pendingSeekSeconds`/续播位置作为初值；与 P1-5 一起改 |
| P2-10 | demuxer cache 512MiB / 无上限日志 | `mpvplayer.cpp:288-289` | 8GB 内存机器 + 4K 高码率时内存压力；NFR-03 8h RSS 需观察；建议 128~256MiB + `demuxer-max-back-bytes` |
| P2-11 | 无显式 `scale/cscale` | `mpvplayer.cpp:215-260` 未设置 | gpu-next 默认 scaler 质量高但低端 GPU 成本高；NFR-02 若达不到 15% 3D，可显式 `scale=spline36`/`cscale=spline36` 或 `bilinear` A/B |
| P2-12 | `config=no` 无高级用户逃生口 | `mpvplayer.cpp:253` | 用户/支持无法用 mpv.conf 覆盖参数；建议“高级：允许用户 mpv.conf（只读附加）”或“复制当前参数到剪贴板” |
| P2-13 | 候选 URL 中的静态直连兜底可能多耗 8s | 见 P1-13 | 与 P1-3 一并做首字节探测 |
| P2-14 | `blend-subtitles=video` 在 gpu-next 下行为待确认 | `mpvplayer.cpp:233`；gpu-next 的 OSD 合成可能忽略该选项 | 不影响字幕显示；但与 mpv CLI 对照 HDR/ASS 截图时需固定同一 vo/选项；S4 验收时记录 |
| P2-15 | ui::MainWindow 仍连接同一批 PlayerPage 播放信号（mock 逻辑） | `reports/playback-integration.md:107`；`app/src/mainwindow.cpp:324-325` 注释 | 真实播放时可能与 Controller 双处理（UI 状态抖动）；建议 mock 逻辑用 `#ifdef`/demo 开关隔离 |
| P2-16 | Ctrl 组合键不转发，无法用 Ctrl+H 运行时切硬解 | `mainwindow.cpp:104-106` | 排障便利性；可加设置页“切换硬解”按钮，或允许 Ctrl+H 透传 |
| P2-17 | mousemove 全量转发给 mpv、窗口 resize 120ms 轮询 | `videohost.cpp:102-105, 148-160`；`mainwindow.cpp:171-175` | 高频 `mouse` 命令增加 mpv 输入线程负担；resize 期间子窗口最多滞后 120ms（黑边抖动）；可节流 16~33ms |
| P2-18 | 无音频设备选择/字幕样式 UI | `settingspage.cpp` 无相关项 | P1-05/P1-04 范围；mpv 默认键（z/Z 等）可用但用户不可见 |

## 5. 逐项检查结论（对应任务清单）

| 检查项 | 结论 | 关键证据 | 备注 |
|---|---|---|---|
| HDR10/PQ tone-map 配置 | **已配置，待实机验证** | `mpvplayer.cpp:224-230`：`target-colorspace-hint=no`、`target-trc=srgb`、`target-prim=bt.709`、`tone-mapping=auto`、`hdr-compute-peak=auto`、`dither-depth=auto` | 设计目标就是 HDR→SDR（requirements 明确不做 HDR 输出）；S1 对照 mpv CLI 验收；无 HDR 检测/提示（non-goal） |
| DV 处理 | **配置依赖 gpu-next，无检测/降级** | `mpvplayer.cpp:218` gpu-next；DV 依赖 libmpv 构建的 libdovi；DV 元数据已解析未使用（`embytypes.h:102-107`） | S2/S3 必测；P5 绿屏时需提示/转码兜底；P7 无专门处理（requirements 允许基础层/错误） |
| d3d11va 硬解失败回退 | **不达标（P0-6）** | `mpvplayer.cpp:222` 强制 `d3d11va`；无 auto-safe；无提示；mpv 会静默回退软解 | NFR-04 要求“自动降级 auto-safe 并提示” |
| ASS 特效字幕配置 | **基本正确** | `blend-subtitles=video`、`sub-ass-override=no`、`embeddedfonts=yes`、`sub-auto=fuzzy`：`mpvplayer.cpp:232-236`；轨道菜单用 mpv `sid`（`mainwindow.cpp:365-374`） | 字体/特效由 libass；S4 验收；外挂字幕缺失（P1-8）；gpu-next blend 行为待确认（P2-14） |
| 音轨切换 | **可用** | `aid` 设置 + 900ms 回退核对：`mainwindow.cpp:375-403`；音轨列表过滤空 codec：`:1341-1380` | 直通缺失（P1-9）；无 alang 语言偏好 |
| 本地文件/文件夹播放 | **可用，有 polish** | `openFile/openFolder/playFiles`：`mainwindow.cpp:656-718`；mpv 加载列表：`mpvplayer.cpp:371-385` | 递归/排序/标题/按钮状态 P2-4/P2-5；无拖放 |
| 播放列表 | **本地可用；Emby 为单 URL** | `mpvplayer.cpp:371-385`；Emby 单条加载 `mainwindow.cpp:1205` | Emby 上下集走队列而非 mpv playlist（因此 P0-1 键无效） |
| 截图/统计 | **统计可用；截图无 UI** | 统计按钮/键：`mpvplayer.cpp:422-425`、`mainwindow.cpp:361-364`；mpv 内置 stats.lua（源码 builtin_lua_scripts）；截图无代码 | 统计依赖 libmpv 内置脚本（已验证源码内置）；截图见 P2-3 |
| key bindings | **大部分可用；N/P、双击 P0；双转发 P1** | `mpvplayer.cpp:75-89`；`mainwindow.cpp:93-148, 543-586, 1473-1496` | 空格/←→/↑↓/F/Esc/I/滚轮音量按 mpv 默认+自定义绑定可用；N/P 与双击见 P0-1/2 |
| 错误提示 | **不达标（P0-4）** | `mainwindow.cpp:1236-1267`、`:286-297` | 无错误码/重试/返回；重试次数超限 |
| 性能（0 拷贝/丢帧） | **配置正确，运行值待测** | `mpvplayer.cpp:218-223`：gpu-next+d3d11+d3d11va（`gpu-context=d3d11` → 零拷贝）；`--wid` 嵌入不经过 Qt 合成 | 需 Windows 验证 `hwdec-current=d3d11va`、Copy<2%、3D 占用；日志 all=v 是额外 CPU/IO（P1-2） |
| 与 Emby 直连 URL 兼容 | **已验证通过**（真实 Emby 4.9.1.90） | `reports/emby-live-validation.md:138-142`：206、Authorization 头、token 不拼 URL；`playbackUrl()` 保留 DeviceId/MediaSourceId/PlaySessionId | 转码/HLS 未真实验证（P1-13）；外部字幕/RequiredHttpHeaders 缺失（P1-8/P2-7） |

## 6. Windows 实机验证建议（30 分钟冒烟，按优先级）

1. **键盘**：播放剧集按 `n/p`（预期立即下一集/上一集；当前 FAIL）；空格只暂停一次（若两次→P1-1）；双击画面（预期进入/退出全屏；当前 FAIL）；滚轮音量、↑↓ 音量、`i` 统计、`f`/Esc 全屏。
2. **续播**：播放 15s → 关闭应用 → Emby Dashboard 看继续观看位置（P0-5）；把条目看到 98% → 重新播放（预期从头，P0-3）；设置里关闭自动续播 → 从头（P0-3，需先加开关）。
3. **错误**：断网/改错服务器 → 播放（预期 3s 内可读错误码 + 重试/返回；当前 FAIL）；本地删掉文件后播放（当前只有状态栏）。
4. **硬解**：S5 播放 → 统计面板 `hwdec-current=d3d11va`（不含 `-copy`）、`current-vo=gpu-next`、`gpu-api=d3d11`；任务管理器 Copy 引擎 <2%。
5. **HDR/DV**：S1（HDR10）不发灰；S2（DV P5）不绿/紫；S3（DV P8）正常；记录 mpv.log 中 DV/libdovi 相关行。
6. **ASS**：S4 特效/字体/位置；关闭/开启字幕 1s 内生效。
7. **直连/转码**：实体库样例 1056815 直连出画面；找一个服务器强制转码的条目（或临时收窄 DeviceProfile）验证 HLS 起播/拖动。
8. **退出日志**：关闭后 grep `%LOCALAPPDATA%/emby-pc/mpv.log` 确认无 `Authorization`/token（P1-2）。

## 7. 关键证据索引

| 结论 | 证据 |
|---|---|
| mpv 选项/硬解/HDR/字幕 | `app/src/mpvplayer.cpp:194-317`（初始化与全部选项）；`:422-425` 统计；`:479-499` END_FILE/日志错误 |
| 输入绑定 | `app/src/mpvplayer.cpp:75-102`（`kInputConf`、写入路径） |
| --wid 宿主/鼠标键盘 | `app/src/videohost.cpp:87-106, 125-160, 162-281` |
| 播放主流程/候选/失败/续播 | `app/src/mainwindow.cpp:680-763, 1038-1267, 1676-1783` |
| 轨道菜单 | `app/src/mainwindow.cpp:1341-1426`；`app/src/ui/playerpage.cpp:1065-1097, 1160-1190` |
| 键盘转发/去重 | `app/src/mainwindow.cpp:93-148, 543-586, 1473-1496` |
| 进度上报/停止 | `app/src/mainwindow.cpp:1676-1754`；`app/src/emby/embyclient.cpp:670-718` |
| 播放 URL/头 | `app/src/emby/embyclient.cpp:178-208, 569-668`；`app/src/emby/deviceprofile.cpp:17-90` |
| 媒体流字段（外挂字幕/DV 未用） | `app/src/emby/embytypes.h:86-135, 160`；`app/src/emby/embytypes.cpp:218-330` |
| 设置页能力 | `app/src/ui/settingspage.cpp:193-215, 417-458` |
| 退出流程 | `app/src/mainwindow.cpp:438-443, 545-549, 1724-1754`；`app/src/main.cpp:210-216`；`app/src/emby/embyclient.cpp:105-113` |
| 验收标准 | `docs/requirements.md` PLY-01~13/SYN-01~03/NFR-01~05；`docs/windows-acceptance.md` O/P/S/W 组 |
| 真实直连验证 | `reports/emby-live-validation.md:122-142` |
| 已知日志 token 风险 | `reports/playback-integration.md:101-109` |

## 8. 限制与说明

- 本次**未修改任何代码**；所有结论来自静态代码 + mpv 官方文档/源码 + 已有真实服务器联调报告。
- 容器无 Windows/GPU/libmpv-2.dll，无法验证 `--wid` 嵌入、D3D11VA zero-copy、HDR/DV tone-map、OSC/stats 渲染、ASS 实际合成；这些均列入 §6。
- 优先级说明：P0 = 阻塞 0.1.0 验收（requirements 的 P0 条目或安全/稳定性）；P1 = 首个稳定版前修；P2 = 可排期。OSC 口径（P1-12）与 HDR/DV 验证风险（§0）未计入 6 个 P0，但需 lead 在批修前决策。

---

报告人：video-analyst（T61）· 2026-11-06
