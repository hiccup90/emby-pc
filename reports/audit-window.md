# 窗口 / 播放器 / 输入 / 全屏 / 尺寸 / Overlay 全面审查

> 任务：task-58（只审查不改代码）
> 基线：`a28e44b`（T56/T57 已合入）
> 审查日期：2026-11-06
> 范围：`app/src/windowframe.*`、`app/src/ui/playerpage.*`、`app/src/ui/mainwindow.*` 的 chrome/全屏/尺寸逻辑、`app/src/videohost.*`、`app/src/mpvplayer.*`

## 结论

- **P0：0 项**（未发现崩溃、数据丢失、安全或核心功能完全不可用的问题）
- **P1：8 项**（影响核心体验/缩放/播放，建议本批集中修）
- **P2：12 项**（体验、健壮性、日志/注释类）

本次只读审查，未修改任何产品代码；所有行号基于 `a28e44b`。

---

## P1（建议同批修复）

### P1-1 播放控制条为不透明顶层窗口，遮住主窗口底边，底部/底角缩放失效

- **文件行**：`app/src/ui/playerpage.cpp:230-253`（`Qt::Tool` overlay）、`802-805`（控制条贴底 108px）、`843-850`（显示/隐藏）；`app/src/windowframe.cpp:420-435`（原生边缘命中）
- **触发条件**：播放模式，控制条可见（默认/暂停），鼠标在主窗口最底部 1–8px 或底部左右角拖动。
- **原因**：控制条是独立 `Qt::Tool` 顶层窗口，Windows 把 `WM_NCHITTEST` 发给控制条窗口，而不是主窗口；控制条自身没有 `WM_NCHITTEST/startSystemResize`，主窗口的 `WM_NCHITTEST` 和 VideoHost 的 6px 兜底都收不到该区域的鼠标。T35 原来靠 5px 黑边露出主窗口边缘；T56 去边后暴露。
- **影响**：控制条可见时，最下边和两个底角不能缩放；自动隐藏后才恢复。用户明确要求“仍可拖边缘缩放”。
- **建议**：控制条底部保留 6–8px 可透传/可命中条（QSS 透明底或单独 hit-test），或给 overlay 安装事件过滤器：距 overlay 底边 <=6px 时调用主窗口 `windowHandle()->startSystemResize(Qt::BottomEdge/LeftEdge|BottomEdge/RightEdge|BottomEdge)`；不要恢复整圈 5px 黑边。

### P1-2 全屏状态仍走 WindowFrame 的 WM_NCHITTEST 边缘命中，屏幕四边出现缩放/错误 cursor

- **文件行**：`app/src/windowframe.cpp:205-221`（`edgeHitTestForTest` 只判断 maximized）、`398-435`（WM_NCHITTEST 未判断 `isFullScreen()`）；`app/src/ui/playerpage.cpp:461-465`（Qt 层全屏禁用）
- **触发条件**：全屏播放时鼠标移到屏幕最上/下/左/右边缘。
- **原因**：全屏窗口 `::IsZoomed(hwnd)` 为 false；`edgeHitTestForTest(...maximized=false)` 仍返回边缘，原生层返回 HTLEFT/HTTOP 等。PlayerPage 自己的 6px 命中虽禁用，但顶层原生命中不受它控制。
- **影响**：全屏下出现 resize cursor，部分系统/鼠标操作可能尝试改变全屏窗口尺寸；与“全屏无边框、不可缩放”预期不符。
- **建议**：WM_NCHITTEST 开头加 `if (m_window->isFullScreen()) { *result = HTCLIENT; return true; }`，或给 `edgeHitTestForTest` 增加 fullscreen 参数并禁止边缘。

### P1-3 最大化按钮顶部 8px 被边缘缩放优先，Snap Layouts/按钮 hover 不完整

- **文件行**：`app/src/windowframe.cpp:420-435`（先 edge 判断）先于 `437-460`（maxRect/HTMAXBUTTON）
- **触发条件**：非最大化窗口，鼠标悬停/点击最大化按钮最上方约 8px（高 DPI 下更大）。
- **原因**：右上角同时命中 Right/Top 边缘，代码先返回 HTRIGHT/HTTOPRIGHT，永远不会到 `HTMAXBUTTON`。
- **影响**：最大化按钮顶部点击无效、Snap Layouts hover 区域变小；用户会感觉“按钮上半部分点不到/不出 Snap”。
- **建议**：在 title 区域内**先判断 maxRect**，命中直接 HTMAXBUTTON；或边缘判断排除 maxRect（以及 min/close 按钮矩形）。

### P1-4 320x180 播放最小尺寸下，顶栏 76px + 控制条 108px 在 180px 高度内重叠

- **文件行**：`app/src/ui/playerpage.cpp:796-805`（topHeight 固定 76、controlHeight 固定 108；顶栏 y=10，控制条 y=height-108）、`817-820`（docked 分支同高）；`app/src/ui/mainwindow.cpp:1332-1339`（T52 允许 320x180）
- **触发条件**：播放模式把窗口拖到 320x180（用户新要求的最小尺寸）。
- **原因**：180px 高时顶栏占 10–86px，控制条占 72–180px，重叠 14px；按钮/进度条互相遮挡，部分按钮可能被顶栏覆盖。
- **影响**：小窗口下控制条不可用/排版挤压，T52“允许 320x180”变成“能缩但不好用”。
- **建议**：小高度时控制条自适应压缩（seek 24px、按钮 32px、减少上下 margin），或 320x180 时隐藏顶栏只保留返回/全屏；至少保证两块不重叠。

### P1-5 MpvPlayer 初始化时多数 setOption 返回值被忽略，关键选项失败无提示

- **文件行**：`app/src/mpvplayer.cpp:213-290`（wid/vo/gpu-api/gpu-context/hwdec/target-*/osc/log-file/cache 等均未检查返回值）
- **触发条件**：libmpv 版本不支持 `gpu-next`/`d3d11va` 选项，或 `wid` 设置失败、`log-file` 不可写。
- **原因**：只检查了 T50 的 `http-header-fields`，其余 `setOption()` 返回 bool 但被丢弃。
- **影响**：可能出现无视频窗口（mpv 另开窗口）、黑屏、无硬解、无 mpv.log，但 UI 无错误提示，排障只能靠猜。
- **建议**：对 `wid/vo/log-file` 等关键选项检查返回值；失败时 `error` 返回并在初始化失败对话框展示；非关键项失败写 qWarning 并继续。

### P1-6 MpvPlayer::loadPlaylist 忽略 loadfile/playlist-pos 失败，无 error 信号

- **文件行**：`app/src/mpvplayer.cpp:371-385`
- **触发条件**：本地文件不存在/权限错误、URL 无效、mpv 内部 loadfile 失败。
- **原因**：`command(...)` 返回值全部未使用，`loadPlaylist` 没有失败回调/信号。
- **影响**：本地播放失败时播放器停在空/旧画面；T48 的 8s watchdog 只覆盖 Emby，本地无提示。
- **建议**：检查每条 `command` 返回值；失败 emit `errorOccurred`；`playlist-pos` 失败也记录；必要时统一走 PlayerController 失败提示。

### P1-7 VideoHost 只给第一个直接子窗口设置 WS_EX_TRANSPARENT，mpv 多子窗口时鼠标可能被原生窗口吃掉

- **文件行**：`app/src/videohost.cpp:305-309`（只取 `GW_CHILD`）、`315-346`（单 child 设置）
- **触发条件**：mpv 创建多个直接子窗口（VO/OSD/统计等），且第一个不是实际视频窗口，或子窗口顺序变化。
- **原因**：`syncMpvChildGeometry` 每 120ms 只对第一个直接 child 调用 `ensureMousePassthrough`；其余 child 没有 `WS_EX_TRANSPARENT`。
- **影响**：鼠标落点被某个原生子窗口接收，VideoHost 收不到 press/move，边缘缩放/T46 拖动/控制条唤醒都可能失效。
- **建议**：用 `EnumChildWindows` 对所有直接 child 设置 passthrough（或按 class/样式筛选 VO 窗口），记录每个 child；child 变化时重新应用。

### P1-8 PlayerPage::tryStartEdgeResize 在 startSystemResize 返回 false 时仍返回 true，边缘点击被吞

- **文件行**：`app/src/ui/playerpage.cpp:481-491`
- **触发条件**：Qt/平台不支持 `startSystemResize`、窗口未就绪或全屏边界状态异常时点击视频边缘。
- **原因**：函数无论 `started` 真假都 `return true`；`mousePressEvent`/eventFilter 随即 accept 并 return，不再转发给 mpv/拖动。
- **影响**：边缘点击无反应（不会播放/暂停，也不会启动拖动），疑似“点了没用”。
- **建议**：`return started`；失败时记录日志并让事件继续走 T46 拖动或 mpv 点击。

---

## P2（可随后统一清理）

1. **PlayerPage::paintEvent 在播放模式整页填黑**  
   `app/src/ui/playerpage.cpp:569-577`。视频 child 通常覆盖，但未来 overlay/透明区域/占位切换会露出整块黑；建议只给 placeholder 黑底，父页面保持透明/背景色。
2. **离开播放模式不 unsetCursor**  
   `app/src/ui/playerpage.cpp:860-888` 未清 cursor；在边缘退出后回主页可能残留 SizeHor/SizeVer。建议 `setPlayerModeActive(false)` 调 `unsetCursor()`。
3. **PlayerPage 顶栏 title/subtitle QLabel 未显式透明，拖动依赖 QLabel 事件冒泡**  
   `app/src/ui/playerpage.cpp:273-276`。当前 QLabel 无 TextInteraction、通常会冒泡到 topBar，但版本/样式变化会回归；建议显式 `WA_TransparentForMouseEvents` 或给标签装 eventFilter，保证标题文字拖动。
4. **控制条时间标签/音量图标未装 eventFilter**  
   `app/src/ui/playerpage.cpp:297-303、356-357`。点击时间文字/音量图标不经过 T51 的 control mouse 日志/auto-hide 重置；建议统一透明或过滤。
5. **SeekSlider 按下即 seek，release/click 可能重复发 seek**  
   `app/src/ui/playerpage.cpp:104-134、426-436`。按下时 onSeek，拖动时每次 move 也 seek，release 再触发 sliderReleased；建议明确“press 立即 seek + move 不重复或 release 只收尾”的策略，避免对服务器/播放内核连续发 seek。
6. **MpvPlayer::shutdown 不清空/通知 UI 状态**  
   `app/src/mpvplayer.cpp:158-169` 只清成员，不 emit position/duration/paused/title；reinitialize 后 UI 可能保留旧进度。建议 shutdown 后 emit pausedChanged(false)、positionChanged(0)、durationChanged(0)、titleChanged({})。
7. **MPV_EVENT_LOG_MESSAGE 任何 error 都 emit errorOccurred，T48 可能把非致命错误当播放失败重试**  
   `app/src/mpvplayer.cpp:489-500`；`app/src/mainwindow.cpp` T48 `errorOccurred` 分支会尝试下一个候选。建议只对 HTTP/loadfile 相关错误或 END_FILE 错误触发重试，其余只写日志。
8. **DWM 属性只在构造/WM_ACTIVATE 应用，未覆盖 WM_SHOWWINDOW/Qt Show**  
   `app/src/windowframe.cpp:275-290、483-486`。Win11 在某些显示/还原路径可能重置 corner/border；建议 `WM_SHOWWINDOW` 后再 apply 一次，并记录 hr。
9. **updateWindowChrome 在 chromeVisible 分支直接 m_titleBar->raise()，未判空**  
   `app/src/ui/mainwindow.cpp:319-335`。当前调用时序 titlebar 已创建，但构造早期 changeEvent 可能触发空指针；建议加 `if (m_titleBar)` 或统一 guard。
10. **WindowFrame::setMinimumWindowSize 只改原生值，不触发重新计算**  
    `app/src/windowframe.cpp:152-156`。当前 T52/T57 由 UI 同步 `setMinimumSize` + resize 兜底；若以后单独调 API 且当前窗口小于新最小值，WM_GETMINMAXINFO 要到下一次拖动才生效。建议同时 `SetWindowPos(...SWP_FRAMECHANGED)` 或在 UI 层继续双写。
11. **T56 注释仍多处写 5px 边距**  
    `app/src/ui/playerpage.cpp:33-36、571-573`、`app/src/ui/playerpage.h:76`；已无 5px layout margin，实际是 6px 边缘命中区，避免后续维护误改。  
12. **loadPlaylist startIndex>0 先 load 第一个再设置 playlist-pos，可能闪一下第一集**  
    `app/src/mpvplayer.cpp:371-385`；建议多文件时先 append 后再设置 playlist-pos，或使用 `loadfile ... append` 后再切。

---

## 已核对通过 / 不需要改的链路

- 播放模式视频区 margin 0、overlay 顶栏/控制条贴边（T56）在正常 1280x760/960x600 尺寸下一致。
- T46 视频/顶栏空白区 >4px startSystemMove 与 T49 TitleBar 拖动链路保留。
- T54 DWM 直角、T56 DWM border/margins 的调用顺序没有互相覆盖（但见 P2-8 的 show 时机）。
- T52 播放模式 320x180 / 普通 800x480 的 Qt + WindowFrame 双写存在；T57 restoreWindowedChrome 的顺序（min → resize → chrome → syncTitleBar → T47 sidebar）正确。
- T35/T31 的 6px 边缘命中在 PlayerPage/VideoHost 正常尺寸下可触发；P1-1/P1-3/P1-8 是边界场景。
