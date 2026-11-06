# Emby PC 全面审查汇总与 v0.1.1 批修计划

> 文档：reports/full-audit.md · v1.0 · 2026-11-06 · 汇总：planner（task-62）
> 输入：T58 [audit-window.md](audit-window.md)、T59 [audit-ui.md](audit-ui.md)、T60 [audit-emby.md](audit-emby.md)、T61 [audit-playback.md](audit-playback.md)；代码基线 commit `a28e44b`，当前版本 0.1.0
> 目标：合并去重四份审查，按 P0/P1/P2 重排；对照 [docs/requirements.md](../docs/requirements.md) 的 P0 条目做覆盖矩阵；按文件/模块给出批修计划与回归测试；判定 v0.1.1 能否发布。

## 0. 结论摘要

**一句话：当前 0.1.0 构建不能直接作为 v0.1.1 发布。**

- 四份审查合并去重后：**10 个代码 P0、27 个 P1、41 个 P2**；另有 **1 个验收口径决策（OSC vs Qt 控制条）** 和 **2 个必须在 Windows 实机完成的 P0 验证项（HDR10 / DV P5/P8）**。
- 最严重的是**两处 token 泄露**：图片 URL 带 `api_key`（A-P0-01）、`mpv.log` 记录 `http-header-fields` 中的完整 token（A-P0-02）。两项直接违反 NFR-05，必须先修。
- 用户最近报的 8 个问题中，**「首页缩放/滚动」与「播放模式 320×180 控制条」仍未修干净**（A-P0-03/A-P0-04）；其余多为已修但需实机复测或有 P1 残留（见 §4）。
- 播放核心另有 6 个 P0：N/P 键切集无效、双击不进应用全屏、续播 >95% 与自动续播开关缺失、错误处理不达标、退出丢 Stopped、硬解失败无降级提示。
- 建议：**先并行修 3 条 P0 车道（播放内核/控制器、UI、决策与文档），再做 Windows P0 验证；P0 全绿后才发 0.1.1。** P1 同批修用户可感知项；P2 放 0.1.2。

| 严重度 | 数量 | 处置 |
|---|---:|---|
| P0（代码） | 10 | 阻塞 v0.1.1，必须全修 |
| P0（决策/文档） | 1 | lead 冻结口径，release-engineer 同步文档 |
| P0（实机验证） | 2 | Windows S1/S2/S3 验收，失败升为代码 P0 |
| P1 | 27 | v0.1.1 建议修完用户可感知项；其余经 lead 批准后可移 |
| P2 | 41 | 0.1.2 及以后清理，不阻塞 v0.1.1 |

---

## 1. 审查输入、方法与去重规则

### 1.1 输入与基线

| 来源 | 报告 | 原始统计 | 基线 |
|---|---|---|---|
| T58 窗口/播放器/输入/全屏/尺寸/overlay | reports/audit-window.md | P0 0 / P1 8 / P2 12 | commit `a28e44b` |
| T59 UI/UX/布局/设置/登录/空态 | reports/audit-ui.md | P0 2 / P1 5 / P2 6 | commit `a28e44b` |
| T60 Emby 集成/鉴权/会话/错误处理 | reports/audit-emby.md | P0 2 / P1 8 / P2 10 | commit `a28e44b` |
| T61 播放兼容/硬解/HDR/字幕/音轨/本地文件 | reports/audit-playback.md | P0 6 / P1 12(+1) / P2 18 | commit `a28e44b` |

基线证据（本容器可复现）：

- `./app/scripts/build-linux.sh` 构建通过（当前工作树，Linux/Qt 6.8）。
- `QT_QPA_PLATFORM=offscreen /tmp/emby-ui-build2/emby-ui-demo --selftest`：**190 PASS / 0 FAIL**。
- Emby 单元测试：`/tmp/lead-emby/emby_tests` 输出 `ALL TESTS PASSED`（T60 记录 95 checks）。
- Windows CI：`.github/workflows/windows-build.yml` 最近一次（T56/T57）构建、打包、VERSIONINFO、安装包通过。

### 1.2 严重度定义（本次统一口径）

| 级别 | 定义 | 发布影响 |
|---|---|---|
| **P0** | 崩溃/数据丢失/安全泄露；核心功能完全不可用；用户已明确报告且仍可复现的回归；直接违反 requirements P0 验收 | 不修完不能发 v0.1.1 |
| **P1** | 核心体验明显受损、边界错误、高频路径可感知；不修可发但用户会继续报 | v0.1.1 建议修；可经 lead 书面批准部分延期 |
| **P2** | 体验/健壮性/日志/维护性问题，有 workaround 或低频 | 0.1.2 起清理 |

### 1.3 去重与改级

- 同一根因跨报告出现只保留一条，标注全部来源；P0/P1 冲突取更高严重度。
- 纯文档口径冲突（OSC vs Qt 控制条）单列为 **A-P0-D1**；只能在 Windows 实机确认的 HDR/DV 验收风险单列为 **A-P0-V1/V2**，不重复计入代码 P0 数量。

| 原始条目 | 处理 |
|---|---|
| T58 P1-4（320×180 顶栏/控制条重叠） | 与 T59 P0-2 合并为 A-P0-04，升 P0 |
| T60 P1-4（退出时 Stopped 异步丢失） | 与 T61 P0-5 合并为 A-P0-09，升 P0 |
| T61 P1-2（mpv.log verbose/未脱敏） | 与 T60 P0-2 合并为 A-P0-02，保留 P0 |
| T61 P1-5（Start 早于 fileLoaded/失败不 Stopped） | 与 T60 P1-2 合并为 A-P1-14 |
| T61 P1-12（OSC vs Qt 控制条口径） | 单列 A-P0-D1（决策） |
| T61 P1-3/P1-4/P1-13 + T58 P2-7 | 合并为 A-P1-21（候选 URL 判定策略） |
| T58 P2-11 + T59 P2-5（5px 注释） | 合并为 P2-UI-10 |
| T60 P2-8（mpv.log 无轮转） | 并入 A-P0-02 修复范围 |
| T61 P2-13（静态兜底多耗 8s） | 并入 A-P1-21 |

---

## 2. P0 问题（发布阻断，10 个代码项）

### A-P0-01 图片请求把 access token 拼进 URL（`api_key`）

- **来源**：T60 P0-1
- **现象/触发**：登录后任何真实海报/背景图请求；URL 形如 `.../Images/Primary?api_key=<token>`。反向代理、抓包、网络日志均可记录。
- **证据**：`app/src/ui/embythumbnailcache.cpp:436-446`；测试固化了行为：`app/src/ui/demo_main.cpp:1794-1807`；文档矛盾：`app/src/emby/README.md:84`、`app/src/ui/README.md`。
- **根因**：图片请求在 `Authorization` 头之外又把 token 加进查询参数。
- **修复**：删除 `api_key` query；只发 `Authorization: MediaBrowser ...`（Emby 可加 `X-Emby-Token` 头）；缓存 key 继续用 token-free 的 `imageUrl()`；同步修正 demo_main 断言、README、`docs/windows-acceptance.md` 的 L-05。
- **验收**：抓包/断言确认图片 URL 无 token；首页/媒体库/详情 poster 不回归；敏感词扫描 0 命中。
- **预估/owner/批次**：0.25d · ui-builder（emby-api 评审）· B1-B

### A-P0-02 `mpv.log` 记录 `http-header-fields` 完整 token，并被诊断包带走

- **来源**：T60 P0-2；T61 P1-2（合并）
- **现象/触发**：mpv 初始化后再更新 Authorization header（切集/继续观看/重登录都会发生），mpv 把 `Set property: http-header-fields="...Token=..."` 写入 `mpv.log`；`exportDiagnostics()` 会把 `mpv.log` 打包给用户回传。
- **证据**：`app/src/mpvplayer.cpp:171-177`（setter）、`:284-287`（`log-file` + `msg-level=all=v`）；`app/src/mainwindow.cpp:725-727`（runtime 更新）、`:1659-1666`（导出诊断）；T60 已在本地 libmpv 0.40 复现，降低 `msg-level` 仍不能消除该条 setter 日志。
- **根因**：token 通过 mpv 属性 setter 注入，而 mpv verbose 日志会记录属性值；日志文件未脱敏、无轮转。
- **修复（推荐 1）**：不在 `log-file` 生效后调用 token setter；token 变化时重新初始化/重建 mpv，在 `log-file` 之前设置 `http-header-fields`。辅以：发行版默认 `msg-level=info`（DEBUG 由设置页开启）、`mpv.log` 大小上限/轮转、导出前 token 脱敏、清理已有含 token 旧日志。
- **必须补的测试**：本地 libmpv + 含 token header 启动 → grep `mpv.log`/诊断包，断言无 token；切换剧集/退出登录后再次检查。
- **验收**：日志与诊断包敏感词扫描 0 命中；正常播放/切集不回归。
- **预估/owner/批次**：0.75d · build-verifier · B1-A

### A-P0-03 首页 section 横向溢出：800/960 宽右侧被裁且不能横向滚动

- **来源**：T59 P0-1（用户已报「首页缩放/滚动」）
- **现象/触发**：1280→960/800 缩放，或播放/全屏返回 800×480。实测 offscreen 800：viewport=546、HomeSectionsContent width=1022、horizontalScrollBar maximum=476，但横向滚动条被关闭；右侧继续观看卡片被裁。
- **证据**：`app/src/ui/homeview.cpp:309-312`、`:667-671`、`:853-868`；preview `06-home-960x600.png`。
- **根因**：`resizeHomeSectionRows()` 对 resume content `setFixedSize(hint)` 后又调 `m_sectionsContent->adjustSize()`，把外层 content 撑得比 viewport 宽；内层 resume 横向滚动无法接管外层。
- **修复**：`resizeHomeSectionRows()` 只算各 section 高度，不对 `m_sectionsContent` 调 `adjustSize()` 撑宽；外层 QScrollArea 宽度始终等于 viewport；resume 横向滚动只作用在内层 `HomeSectionScroll`；`layoutPreviewRow()` 最小列数从 6 降到 2–4，按 available 动态算卡宽。
- **必须补的测试**：`demo_main --selftest` 增加 800/960/1280 三档断言：content 宽度等于 viewport、横向 scroll maximum 为 0、卡片 rect 不超出 viewport。
- **验收**：800/960 无横向裁切/无横向滚动条；继续观看卡片可见；1280 不回归。
- **预估/owner/批次**：0.5d · ui-builder · B1-B

### A-P0-04 播放模式允许 320×180，但控制条/顶栏在该尺寸溢出、遮挡画面

- **来源**：T59 P0-2 + T58 P1-4（合并升 P0；用户已报「播放模式最小尺寸」）
- **现象/触发**：播放模式拖到 320×180。控制条固定按钮总宽约 394px + margins 超过 320；顶栏 10–86px + 控制条 72–180px 在 180px 高度内重叠 14px；部分按钮被裁/被顶栏覆盖。
- **证据**：`app/src/ui/playerpage.cpp:292-314`（margins/spacing）、`:320`（播放按钮 48）、`:349`（字幕/音轨/统计 36）、`:362`（音量 96）、`:797-805`（topHeight 76 + controlHeight 108）；`app/src/ui/mainwindow.cpp:1332-1336`（T52 允许 320×180）。
- **根因**：控制条/顶栏固定尺寸布局，没有 compact 断点；T52 只放开最小尺寸，没有同步重排 UI。
- **修复（二选一，推荐 A）**：A. 播放模式最小尺寸提高到 640×360（仍远小于 800×480，满足“能缩小”诉求）；B. 保留 320×180，增加 compact 布局：压缩 margins、隐藏音量滑杆/次要按钮进菜单、控制条高度 ≤72、顶栏单行化。无论选哪个，都补 320/640 的按钮 rect 与重叠断言。
- **验收**：320×180（若保留）或 640×360 下按钮不重叠/不被裁；顶栏与控制条不重叠；全屏/返回主页正常。
- **预估/owner/批次**：0.75–1d · ui-builder · B1-B

### A-P0-05 Emby 播放时 N/P 键不能切上下集

- **来源**：T61 P0-1
- **现象/触发**：任意剧集播放中按 `n`/`p` 无反应；只能点 Qt 控制条的上一集/下一集。
- **证据**：`app/src/mpvplayer.cpp:75-89`（`n playlist-next`/`p playlist-prev`）；`app/src/mainwindow.cpp:1195-1208`（Emby 每次只 load 一个 URL）；`:330-349`（只有按钮路径，无 script-message）。
- **根因**：mpv 的 N/P 操作 mpv 播放列表，而 Emby 播放列表只有当前 1 项；Qt 上下集逻辑没有接 mpv 键事件。
- **修复**：`kInputConf` 增加 `n script-message app-next-episode`、`p script-message app-prev-episode`（保留本地列表兜底）；MpvPlayer 处理 client-message 并发信号；PlayerController 复用 `stepEmbyEpisode(±1)`；队列未建好时走现有 `requestEpisodeSiblings()`。
- **验收**：剧集播放中 N/P 1 秒内切集；跨季正常；最后一集提示；本地文件 N/P 仍可用。
- **预估/owner/批次**：0.25d · build-verifier · B1-A

### A-P0-06 双击视频不能进入应用全屏

- **来源**：T61 P0-2
- **现象/触发**：播放中双击画面 → 无反应，或 mpv 对子窗口执行 `cycle fullscreen`，Qt 控制条/顶栏状态错乱，不进入 `ui::MainWindow::toggleFullscreen()`。
- **证据**：`app/src/videohost.cpp:198-231`（双击转发 mpv）；`app/src/mpvplayer.cpp:75-89`（未覆盖 `MBTN_LEFT_DBL`）；`app/src/ui/playerpage.cpp:696-700`（只 resetWindowDrag）。
- **根因**：mpv 内置 `MBTN_LEFT_DBL cycle fullscreen` 绑定未被覆盖，操作的是 `--wid` 子窗口；Qt 侧没有双击全屏入口。
- **修复**：`kInputConf` 增加 `MBTN_LEFT_DBL script-message app-toggle-fullscreen`；保留 `mouse ... double` 转发；PlayerController 收到后调 `m_window->toggleFullscreen()`；确保再次双击可退出。
- **验收**：窗口态/全屏态双击都能正确切换；全屏下控制条可呼出；状态与 F/Esc 一致。
- **预估/owner/批次**：0.25d · build-verifier · B1-A

### A-P0-07 续播缺少 >95% 重置与“自动续播”开关

- **来源**：T61 P0-3
- **现象/触发**：① 已看 98% 的条目点播放仍从 98% 开始（应从头）；② 用户想关闭续播只能手动拖回 0，设置里没有开关。
- **证据**：`app/src/mainwindow.cpp:783-788`（电影用 progress 估算）、`:812-813`（继续观看用 playbackPositionTicks）、`:855`/`:926`/`:1025`（队列起点）、`:1165`（无条件用 startPositionTicks）、`:486-509`（没有 autoResume）；全仓库无 0.95 判断；`app/src/ui/settingspage.cpp:193-215` 无开关。
- **根因**：续播入口分散在电影/继续观看/队列三条路径，没有统一规则函数；设置页未定义开关。
- **修复**：新增 `player/autoResume`（默认 true）和设置页开关；统一 `resumeTicksFor(item, duration)`：关闭 → 0；`position >= 0.95 * duration` → 0；否则原值；电影/继续观看/队列/自动下一集统一调用；设置变更即时生效。
- **验收**：98% 条目从头；关闭开关后从头；正常 10 分钟位置仍从 10:00±2s 续播；重启设置保持。
- **预估/owner/批次**：0.5d · build-verifier（controller）+ ui-builder（设置 UI）· B1-A/B1-B

### A-P0-08 播放错误处理不达标：无错误码/重试/返回，重试次数超限

- **来源**：T61 P0-4（与用户已报「实体视频/播放失败」相关）
- **现象/触发**：断网、服务器 403、文件被删、解码失败 → 只弹“播放失败，已记录诊断…”（仅“确定”）或状态栏 5 秒；无错误码/重试/返回；候选最多 4 个（3 次重试）超过 PLY-12“最多 2 次”；本地文件错误无弹窗。
- **证据**：`app/src/mainwindow.cpp:1236-1267`（失败弹窗）、`:1210-1224`（候选循环）、`:286-297`（本地错误只状态栏）、`:1253`（http-status 只读日志）。
- **根因**：错误处理是单一兜底路径，未按错误类型映射 UI，也没有统一重试计数与返回动作；本地/Emby 两条路径行为不一致。
- **修复**：建立 `PlaybackError` 映射（http 403/404/500、mpv load/decode、超时、本地文件）；弹窗含“错误码 + 建议 + 重试 + 返回”；重试计数 ≤2；本地文件走同一错误框；失败时停会话（见 A-P1-14）。
- **验收**：5 类错误 3 秒内出现可读错误；重试 ≤2；返回退出播放页；应用不崩；重试成功可正常播放。
- **预估/owner/批次**：1d · build-verifier · B1-A

### A-P0-09 正常退出丢失最后一次 Stopped/进度

- **来源**：T61 P0-5 + T60 P1-4（合并升 P0）
- **现象/触发**：播放中直接关窗口/退出；`aboutToQuit` 里 `reportPlaybackStopped()` 只发异步 POST，随后事件循环结束，`EmbyClient` 析构 abort 在途请求；服务端最后进度可能停在 10 秒前，续播位置不更新。
- **证据**：`app/src/mainwindow.cpp:438-443`、`:478-484`、`:545-549`、`:1724-1754`；`app/src/main.cpp:210-216`（exec 返回后直接关日志）；`app/src/emby/embyclient.cpp:105-113`（析构 abort）。
- **根因**：退出路径没有等待异步 Stopped 完成，也没有持久化“待上报”状态。
- **修复**：退出前有界同步等待（`QEventLoop` + 1.5s 超时，期间不销毁 EmbyClient）；或把 Stopped 放独立 QNAM/同步 socket，析构前 `waitForFinished`；至少持久化最后位置并在下次启动补报；保证 closeEvent/aboutToQuit/析构只上报一次。
- **验收**：播放 15 秒→关闭→Emby Dashboard 的继续观看位置误差 ≤2 秒；快速关窗 5 次不挂起/不崩溃；无重复 Stopped。
- **预估/owner/批次**：0.5d · build-verifier · B1-A

### A-P0-10 硬解失败无 auto-safe 降级与提示

- **来源**：T61 P0-6
- **现象/触发**：Windows 强制 `hwdec=d3d11va`；硬解不可用（驱动/GPU/片源）时 mpv 静默回退软解，用户只看到高 CPU/掉帧；初始化失败只弹“播放内核初始化失败”，播放中无任何提示。
- **证据**：`app/src/mpvplayer.cpp:219-223`；全仓库 `auto-safe` 0 命中；`app/src/mainwindow.cpp:749-757`（仅初始化失败）、`:529-541`（hwdec 只读显示）。
- **根因**：没有降级链和运行时 hwdec-current 检查，也没有用户可见的“已回退软解”提示。
- **修复**：初始化/加载后检查 `hwdec-current`：为 `no`/含 `-copy`/空 → 播放页状态栏或 OSD 提示“硬件解码不可用，已回退软解”；实现 d3d11va → auto-safe → 软解降级链（运行时切换需 Windows 验证）；设置页加“硬解模式：自动/优先 d3d11va/关闭”（默认自动）。
- **验收**：正常机器 `hwdec-current=d3d11va`；人为禁用/不支持时能自动回退并提示；设置页可切模式；播放不崩。
- **预估/owner/批次**：0.5d · build-verifier（+ ui-builder 设置项）· B1-A/B1-B

### A-P0-D1 验收口径冲突：mpv OSC vs Qt 控制条（待 lead 决策）

- **来源**：T61 P1-12
- **现象**：代码默认 `osc=no`，用 Qt 顶层 overlay 控制条（T18/T45/T51/T55）；但 `docs/windows-acceptance.md` 的 O-01~O-08 和 requirements PLY-07 仍按 mpv OSC 写；旧报告 `reports/playback-integration.md:12` 也写“隐藏 Qt 控制条、由 OSC 提供控制”（已过时）。按现验收文档执行会全部 FAIL。
- **建议（推荐）**：**保留 Qt 控制条**，OSC 降级为 `EMBY_PC_MPV_OSC=1` 的调试选项；同步修改 requirements PLY-07 的技术前提、windows-acceptance O 组、README、decisions 记录（v4）。理由：Qt overlay 已实现并经过 T51/T55 修复，不依赖 libmpv 是否带 osc.lua，跨发行包更稳。
- **备选**：若必须 OSC，则默认 `osc=yes` 并去掉 Qt 控制条，工作量和回归风险显著增加（约 1d+）。
- **决策人/批次**：lead 决策 · release-engineer 改文档 · B1-C

### A-P0-V1 / A-P0-V2 Windows 实机验证门禁（HDR10 / DV P5/P8）

- **来源**：T61 §0 的 2 个 P0 验证风险
- **V1**：S1 HDR10 4K HEVC 10bit → 不发灰、不偏色；与同机 mpv CLI 参考截图一致。配置已就绪（`target-colorspace-hint=no`、`target-trc=srgb`、`target-prim=bt.709`、`tone-mapping=auto`、`hdr-compute-peak=auto`）。
- **V2**：S2 DV P5、S3 DV P8.1/8.4 → 不绿屏/紫屏；DV P5 依赖 libmpv 构建的 libdovi；P7 按 HDR10 基础层处理、不保证颜色。
- **验证方式**：按 [docs/windows-acceptance.md](../docs/windows-acceptance.md) S 组 + 同机 mpv CLI 对照；记录 `hwdec-current`、`current-vo`、mpv 版本/libdovi 信息、截图。
- **判定**：任一失败 → 升为代码 P0（优先检查 libmpv 构建、`vo=gpu-next`、tone-map 选项、Windows HDR 模式），v0.1.1 不得发布。
- **owner/批次**：video-analyst + 用户 Windows 机器 · B1-D

---

## 3. P1 问题（27 项，v0.1.1 建议同批修用户可感知项）

> 说明：P1 不全部是发布阻断；但 §4 用户已报项和下列标 ★ 的高频路径建议 v0.1.1 前修完。其余经 lead 批准可移 0.1.2。

| ID | 来源 | 现象/触发 | 文件:行 | 根因/修复 | 预估 | 批次 |
|---|---|---|---|---|---|---|
| A-P1-01 ★ | T58 P1-1 | 控制条可见时，主窗口最底部 1–8px 与底角不能缩放 | playerpage.cpp:230-253,802-805,843-850；windowframe.cpp:420-435 | 控制条是独立 `Qt::Tool` 顶层窗口，原生 NCHITTEST 发给它；给 overlay 底部 6–8px 做透传或 startSystemResize，不恢复 5px 黑边 | 0.5d | B2-B |
| A-P1-02 ★ | T58 P1-2 | 全屏时鼠标到屏幕四边出现 resize cursor/尝试改尺寸 | windowframe.cpp:205-221,398-435；playerpage.cpp:461-465 | WM_NCHITTEST 未判断 `isFullScreen()`；开头直接返回 HTCLIENT | 0.25d | B2-A |
| A-P1-03 ★ | T58 P1-3 | 最大化按钮顶部约 8px 被边缘缩放优先，点击/Snap hover 不完整 | windowframe.cpp:420-435 先于 437-460 | 边缘判断先于 HTMAXBUTTON；title 区域内先判 max/min/close rect | 0.25d | B2-A |
| A-P1-04 | T58 P1-5 | libmpv 关键 setOption 失败无提示，可能黑屏/另开窗口 | mpvplayer.cpp:213-290 | 返回值被丢弃；wid/vo/log-file 等关键项失败应返回初始化错误，非关键项 qWarning | 0.25d | B2-A |
| A-P1-05 | T58 P1-6 | 本地 loadfile 失败无 error，停在旧画面 | mpvplayer.cpp:371-385 | loadfile/playlist-pos 返回值未用；失败发 errorOccurred | 0.25d | B2-A |
| A-P1-06 | T58 P1-7 | mpv 多直接子窗口时只有一个设 WS_EX_TRANSPARENT，鼠标被原生窗口吃掉 | videohost.cpp:305-309,315-346 | 只处理 GW_CHILD 第一个；用 EnumChildWindows 给所有直接 child 设置并记录变化 | 0.25d | B2-A |
| A-P1-07 | T58 P1-8 | startSystemResize 失败时仍 return true，边缘点击被吞 | playerpage.cpp:481-491 | 无论 started 都 return true；改为 return started，失败继续走拖动/mpv | 0.1d | B2-B |
| A-P1-08 ★ | T59 P1-1 | 首页真实数据失败/加载中无错误/重试，section 空白 | homeview.cpp:638-645,871-897；ui/mainwindow.cpp:956-959 | section 级 error 直接隐藏；增加 banner/EmptyState + 重试，加载 skeleton | 0.5d | B2-B |
| A-P1-09 | T59 P1-2 | 首页卡片只能鼠标，键盘无法打开媒体 | homeview.cpp:27-140,122-232 | 卡片只有 mousePress；改 QAbstractButton/StrongFocus + Enter/Space + focus QSS + accessibleName | 0.5d | B2-B |
| A-P1-10 ★ | T59 P1-3 | 800×480 设置页媒体库行控件横向溢出，排序/显示按钮被裁 | settingspage.cpp:110,145,638-668 | 固定 nav/控件宽超过内容区；窄窗口换行或收缩 nav，补 800 断言 | 0.5d | B2-B |
| A-P1-11 | T59 P1-4 | 播放器图标按钮无 accessibleName，overlay NoFocus | playerpage.cpp:65-67,245,331-347 | 补 setAccessibleName/tooltip 复用；overlay 保留有限键盘焦点；快捷键可操作 | 0.5d | B2-B |
| A-P1-12 | T59 P1-5 | 登录框固定 620 宽、无密码显示切换 | logindialog.cpp:40-70,92-106 | 低分辨率体验差；宽度 520/自适应 + 密码显隐按钮 | 0.25d | B2-B |
| A-P1-13 | T60 P1-1 | 退出登录/本地播放后 mpv 仍保留旧 Authorization header | mainwindow.cpp:619-628,680-742 | 只 stop 不清理 header；logout/本地播放前清空 header，纳入 MpvPlayer 状态 | 0.25d | B2-A |
| A-P1-14 ★ | T60 P1-2 + T61 P1-5 | 播放失败/Start 失败后仍“正在上报”，服务端幽灵会话/每 10s 发 Progress | mainwindow.cpp:1146-1180,1236-1267,1689-1697 | Start 先于 fileLoaded，失败不 Stopped；统一 session 收尾：停 timer、清 m_reporting、补发 Stopped、Start 成功后再启动 timer | 0.5d | B2-A |
| A-P1-15 ★ | T60 P1-3 | 播放中 401/403 只弹登录框，不停进度 timer/不清理旧会话 | mainwindow.cpp:1700-1721,1785-1792 | 抽出 handleSessionExpired：停 timer、尽力 Stopped、清 token/会话、登录框防重 | 0.25d | B2-A |
| A-P1-16 | T60 P1-5 | 服务端绝对 URL 跨主机时仍带全局 Authorization，token 可能发给第三方/CDN | embyclient.cpp:585-606；mainwindow.cpp:1098-1112；mpvplayer.cpp:262-282 | resolveMediaUrl 保留任意 host；校验 host:port 与 baseUrl 同源，CDN 白名单；跨源不带 token | 0.25d | B2-C |
| A-P1-17 | T60 P1-6 | token 明文 JSON 存 QSettings | tokenstore.cpp:26-31 | 本机可读；Windows DPAPI/Credential Manager，迁移旧明文；非 Windows 受限文件 | 1d | B2-B |
| A-P1-18 | T60 P1-7 | 崩溃 minidump 含 MiniDumpWithDataSegs，可能带 token | crashhandler.cpp:177-180 | 发布版改 MiniDumpNormal 或限制访问；崩溃文本先脱敏 | 0.5d | B2-A |
| A-P1-19 | T60 P1-8 | 启动恢复/手动登录/退出无代际保护，迟到回调覆盖新会话 | ui/mainwindow.cpp:1488-1533,1566-1603,1605-1638 | 加 m_sessionGeneration，restore/login/logout 递增；旧回调丢弃；client abortAll | 0.25d | B2-B/B2-C |
| A-P1-20 ★（待 Windows 实测） | T61 P1-1 | 键盘可能双转发（空格/F/I 触发两次）；lead 已列为待决 | mainwindow.cpp:438-453,554-560,1473-1495 | app+window 两层过滤器都转发且去重参数都传 0；只保留一层或按事件指针去重；需 Windows 5 分钟实测 | 0.25d | B2-A |
| A-P1-21 ★ | T61 P1-3/P1-4/P1-13 + T58 P2-7 | 8s 固定 watchdog 误判慢起播；任意 mpv error 日志切候选；无 HEAD 探测，坏候选各耗 8s | mainwindow.cpp:199-203,1109-1112,1206-1234,291-294；mpvplayer.cpp:489-499 | 候选判定策略不可靠；首次起播 25–30s，file-loaded 即停；只对 END_FILE/http 4xx5xx 切候选；坏 URL 先 HEAD/首字节探测 | 1d | B2-A |
| A-P1-22 | T61 P1-6 | 音量记忆不生效：调到 30 重启回 80 | settingspage.cpp:442-443；mainwindow.cpp:490-492,357-359 | 只读不写 player/volume；volumeChanged 防抖写 QSettings（rememberVolume=true），退出保存 | 0.25d | B2-A |
| A-P1-23 ★ | T61 P1-7 | 多显示器全屏不在鼠标所在屏 | ui/mainwindow.cpp:1452 | 直接 showFullScreen；全屏前 setScreen(screenAt(QCursor::pos()))，恢复原屏 | 0.25d | B2-B |
| A-P1-24 | T61 P1-8 | Emby 外挂字幕不加载，字幕菜单只有内嵌轨 | embytypes.h:98-127（解析未用）；mainwindow.cpp:1382-1426 | 播放前从 MediaStreams 过滤 Subtitle && IsExternal，resolveMediaUrl + mpv sub-add + Authorization | 1d | B2-A/B2-C |
| A-P1-25 | T61 P1-9 | 音频直通缺失，Atmos/TrueHD 无法点亮 | 全仓库无 audio-spdif；settingspage 无项 | P1 功能；设置开关映射 audio-spdif + channels，失败回退 PCM 并提示 | 0.5d | B2-A/B2-B |
| A-P1-26 | T61 P1-10 | DV 未检测/无降级提示；P5 绿屏风险无用户提示 | embytypes.h:102-107（未用）；mpvplayer.cpp:224-230 | 播放前读 extendedVideoType；DV 片源记日志/状态栏；颜色异常提示可切转码 | 0.5d | B2-A |
| A-P1-27 | T61 P1-11 | track-list 无观察，转码/外挂字幕/切轨后菜单不刷新 | mainwindow.cpp:259-261,1382-1426 | 只在 fileLoaded+150/700ms 拉一次；observe track-list/sid/aid 去抖刷新 | 0.25d | B2-A |

> Owner 映射：B1-A/B2-A/B3-A = build-verifier；B1-B/B2-B/B3-B = ui-builder；B2-C/B3-C = emby-api；B1-C = lead + release-engineer。

> ★ = 建议 v0.1.1 必修；其余 P1 可由 lead 批准延后到 0.1.2。A-P1-20 需 Windows 实测是否真的双触发（lead 已列为待决）。

## 3.5 P2 问题（41 项，0.1.2 清理，不阻塞 v0.1.1）

> P2 合并去重后按模块分组；每条保留来源与修复建议。P2 不进入 v0.1.1 发布门禁，但 B3 批量清理时按模块 owner 执行。单条 P2 预估均为 0.1–0.5 人日，批次总量见 §6.5；owner 按 §6.1 文件所有权映射。

### 3.5.1 P2-WIN（windowframe / videohost）

| ID | 来源 | 现象/根因 | 位置 | 修复 | 批次 |
|---|---|---|---|---|---|
| P2-WIN-01 | T58 P2-8 | DWM 属性只在构造/WM_ACTIVATE 应用，Win11 某些显示/还原路径可能重置 corner/border | windowframe.cpp:275-290,483-486 | WM_SHOWWINDOW 后再 apply 一次并记录 hr | B3-A |
| P2-WIN-02 | T58 P2-10 | setMinimumWindowSize 只改原生值，不触发重算；当前 UI 双写兜底 | windowframe.cpp:152-156 | SetWindowPos(SWP_FRAMECHANGED) 或继续保持 UI 双写并注释 | B3-A |
| P2-WIN-03 | T61 P2-17 | mousemove 全量转发 mpv、resize 120ms 轮询；输入线程负担/黑边抖动 | videohost.cpp:102-105,148-160；mainwindow.cpp:171-175 | 节流 16–33ms；resize 用事件+定时兜底 | B3-A |
| P2-WIN-04 | T61 P2-1 | 鼠标中键/右键编号疑似互换（mpv 约定 0 左、1 右、2 中） | videohost.cpp:171-184 | 用 mpv input-test/OSC 右键菜单验证后改映射 | B3-A |

### 3.5.2 P2-UI（app/src/ui）

| ID | 来源 | 现象/根因 | 位置 | 修复 | 批次 |
|---|---|---|---|---|---|
| P2-UI-01 | T58 P2-1 | PlayerPage paintEvent 在播放模式整页填黑，未来透明/占位会露黑块 | playerpage.cpp:569-577 | 只给 placeholder 黑底，父页面保持背景色 | B3-B |
| P2-UI-02 | T58 P2-2 | 离开播放模式不 unsetCursor，边缘退出后可能残留 SizeHor/Ver | playerpage.cpp:860-888 | setPlayerModeActive(false) 调 unsetCursor | B3-B |
| P2-UI-03 | T58 P2-3 | 顶栏 title/subtitle QLabel 未显式透明，拖动依赖事件冒泡 | playerpage.cpp:273-276 | 显式 WA_TransparentForMouseEvents 或 eventFilter | B3-B |
| P2-UI-04 | T58 P2-4 | 控制条时间标签/音量图标未装 eventFilter，点击不重置 auto-hide | playerpage.cpp:297-303,356-357 | 统一透明或过滤 | B3-B |
| P2-UI-05 | T58 P2-5 | SeekSlider 按下即 seek，move/release 可能重复 seek | playerpage.cpp:104-134,426-436 | 明确 press 立即 seek + move 不重复/release 收尾 | B3-B |
| P2-UI-06 | T59 P2-1 | 顶栏只有 260px 小面板，与控制条全宽不一致 | playerpage.cpp:797-800 | 顶栏与控制条同宽或至少 50%，透明区仍可点 | B3-B |
| P2-UI-07 | T59 P2-2 | 设置页有“界面”分组但左侧无“界面”入口 | settingspage.cpp:126-130,239-252 | 并入服务器分组标题或新增通用/界面分类 | B3-B |
| P2-UI-08 | T59 P2-3 | 设置页可见 Mock（本地演示数据）/临时明文存储文案 | settingspage.cpp:375,518；tokenstore.cpp:42 | 正式构建隐藏或移到关于/诊断；token 显示改 DPAPI/QSettings | B3-B |
| P2-UI-09 | T59 P2-4 | focus 2px 会挤压 36×36 固定按钮内部布局 | dark.qss:1087-1092；playerpage.cpp:65,349 | focus 用 inset outline 或预留 padding | B3-B |
| P2-UI-10 | T58 P2-11 + T59 P2-5 | 注释仍写 5px 边距，实际 6px 命中区 | playerpage.cpp:34,213,463,572 | 统一注释为 6px 命中区、布局 margin 0 | B3-B |
| P2-UI-11 | T59 P2-6 | 首页 section 卡片标题无省略/换行/tooltip，窄列直接裁切 | homeview.cpp:151-159；QSS HomeSectionCardTitle | elide 或 wordWrap + tooltip | B3-B |
| P2-UI-12 | T61 P2-16 | Ctrl 组合键不转发，无法用 Ctrl+H 运行时切硬解 | mainwindow.cpp:104-106 | 设置页加“切换硬解”按钮或允许 Ctrl 透传 | B3-B |
| P2-UI-13 | T61 P2-15 | ui::MainWindow 仍连接同一批 PlayerPage 播放信号（mock 逻辑），真实播放可能双处理 | mainwindow.cpp:324-325；reports/playback-integration.md:107 | mock 逻辑用 demo 开关/#ifdef 隔离 | B3-B |
| P2-UI-14 | T61 P2-18 | 无音频设备选择/字幕样式 UI | settingspage.cpp | P1-05/P1-04 范围；后续加 | B3-B |
| P2-UI-15 | T60 P2-4 | 详情页 Series 整剧 episode 查询无 Limit，大剧集内存/延迟不可控 | ui/librarydata.cpp:902-919 | 按季/分页拉取或设合理上限+“加载更多” | B3-B |

### 3.5.3 P2-CORE（mpvplayer / PlayerController）

| ID | 来源 | 现象/根因 | 位置 | 修复 | 批次 |
|---|---|---|---|---|---|
| P2-CORE-01 | T58 P2-6 | MpvPlayer::shutdown 不清空/通知 UI，reinitialize 后可能保留旧进度 | mpvplayer.cpp:158-169 | shutdown emit paused/position/duration/title 重置 | B3-A |
| P2-CORE-02 | T58 P2-12 | loadPlaylist startIndex>0 先 load 第一集再设 pos，可能闪一下 | mpvplayer.cpp:371-385 | 先 append 再设 playlist-pos，或 loadfile 后切 | B3-A |
| P2-CORE-03 | T61 P2-3 | 截图无 UI，mpv 默认存 CWD（Program Files 不可写会静默失败） | 全仓库无 screenshot 代码 | 设置 screenshot-directory 到图片目录 + 状态提示/按钮 | B3-A |
| P2-CORE-04 | T61 P2-4 | 本地文件夹非递归、QDir::Name 排序不自然、扩展名白名单不全 | mainwindow.cpp:52-68 | 可选递归、QCollator 自然排序、补 m2ts/mts/flv | B3-A |
| P2-CORE-05 | T61 P2-5 | 本地播放列表标题卡第一集、上下集按钮状态不更新 | mainwindow.cpp:709,704-708 | 观察 playlist-pos，切文件更新标题/按钮 | B3-A |
| P2-CORE-06 | T61 P2-6 | 多版本 MediaSource 无选择，只取第一个 DirectPlay | embytypes.cpp:517-524 | 返回所有可用 source 并按 DirectPlay→DirectStream→Transcode 排序 | B3-A |
| P2-CORE-07 | T61 P2-8 | CanSeek 固定 true，未来直播/不可 seek 源会误导服务器 | embytypes.h:328 | 用实际 CanSeek/源能力回填 | B3-A |
| P2-CORE-08 | T61 P2-9 | PlaybackStart 的 PositionTicks 为 0（续播时） | mainwindow.cpp:1685 | 用 pendingSeek/续播位置作为初值，与 A-P1-14 一起改 | B3-A |
| P2-CORE-09 | T61 P2-10 | demuxer cache 512MiB 对 8GB 机器/4K 压力大；mpv.log 无上限 | mpvplayer.cpp:288-289 | 128–256MiB + demuxer-max-back-bytes；日志轮转并入 A-P0-02 | B3-A |
| P2-CORE-10 | T61 P2-11 | 未显式 scale/cscale，低端 GPU 默认 scaler 成本高 | mpvplayer.cpp:215-260 | 按 GPU 档位显式 spline36/bilinear A/B | B3-A |
| P2-CORE-11 | T61 P2-12 | config=no 无高级用户逃生口 | mpvplayer.cpp:253 | 高级设置允许只读附加 mpv.conf 或复制当前参数 | B3-A |
| P2-CORE-12 | T61 P2-14 | blend-subtitles=video 在 gpu-next 下行为待确认 | mpvplayer.cpp:233 | S4 验收时与 mpv CLI 对照并记录；不影响功能 | B3-A |
| P2-CORE-13 | T61 P2-2 | 滚轮/↑↓ 改音量后 Qt 滑条不同步 | playerpage.cpp:417；mpvplayer 未 observe volume | 观察 volume 回填滑条或控制器 setVolume 同步 | B3-A/B3-B |
| P2-CORE-14 | T60 P2-3 | 剧集队列固定 Limit=500，新 series 请求失败不清旧队列 | mainwindow.cpp:824-831,845-857 | 分页拉全/按目标集定位；请求开始清空/禁用旧队列 | B3-A |

### 3.5.4 P2-EM（app/src/emby）

| ID | 来源 | 现象/根因 | 位置 | 修复 | 批次 |
|---|---|---|---|---|---|
| P2-EM-01 | T60 P2-1 | token 清理键名不完整（token/accessToken/apiKey/X-MediaBrowser-Token） | embyclient.cpp:62-73 | 统一大小写不敏感 token 参数集合 + 测试 | B3-C |
| P2-EM-02 | T60 P2-2 | 切换 profile 不取消在途请求；loadRealViews 无 generation | embyclient.cpp:115-128；ui/mainwindow.cpp:902-926 | 请求代际/按 profile 取消；与 A-P1-19 一起 | B3-C |
| P2-EM-03 | T60 P2-5 | preferredMediaSource 只选第一个 DirectPlay 源 | embytypes.cpp:517-524 | 返回所有 source 候选，失败切换 source | B3-C |
| P2-EM-04 | T60 P2-6 | DeviceProfile 字幕只声明 ass/srt，容器/编码覆盖偏窄 | deviceprofile.h:39-57；deviceprofile.cpp:68-83 | 补 PGS/SUP 与 mov/avi/wmv/m2ts/mpeg2video/vc1 等 | B3-C |
| P2-EM-05 | T60 P2-7 | 元数据 GET 无重试/退避，瞬时网络错误只能手动重试 | embyclient.cpp:337-378 | 幂等 GET 1–2 次指数退避；POST 不盲目重试 | B3-C |
| P2-EM-06 | T60 P2-9 | 首次探测 Jellyfin 前默认 Emby 类型，先发 legacy 头 | embyclient.cpp:238-242；logindialog.cpp:160-175 | 未确认类型只发标准 Authorization；public info 可不带认证头 | B3-C |
| P2-EM-07 | T60 P2-10 | 测试缺口：mpv.log token、图片 URL、失败 Stopped、401、同源、Jellyfin 头 | app/src/emby/tests；demo_main.cpp | 新增 fake server + 敏感词扫描 + 同源/401/停止用例 | B3-C |
| P2-EM-08 | T61 P2-7 | RequiredHttpHeaders 已解析但未使用，部分 STRM/IPTV 源会 401/403 | embytypes.h:160 | 合并到 A-P1-24 的 URL/头处理 | B3-C |

---

## 4. 用户已报问题「可能未修干净」复核

| 用户现象 | 历史修复 | 本次审查证据 | 结论/后续 |
|---|---|---|---|
| 窗口边框/圆角 | T54 DWM 直角、T56 去 1px 边框/黑边 | T58 通过构造/WM_ACTIVATE 应用 DWM 属性；WM_SHOWWINDOW 未重应用（P2-WIN-01）；DWM 权限失败只日志 | **代码已修，需 Win10/Win11 实机复测**；P2 补丁不阻塞 |
| 全屏退出残留/无缩放光标 | T46/T47/T57 状态机与 chrome 恢复 | T46/T57 的 showNormal+NoState 顺序通过；但全屏时 WM_NCHITTEST 仍返回边缘（A-P1-02） | **部分修干净**：状态恢复已修，全屏边缘热区未修 |
| 全屏/播放模式退出侧边栏变形 | T47/T57 splitter/sidebar 恢复 | T58/T59 复核 splitter 240→240、clamp 180–320、恢复顺序正确；selftest 190 PASS | **已修**；侧边栏本身无残留，首页滚动另见 A-P0-03 |
| 标题栏 close/min/max 不见（chrome） | T57 restoreWindowedChrome | T58 复核 min→resize→chrome→syncTitleBar→sidebar 顺序正确；T59 320→800 按钮在窗口内 | **已修**；P2-WIN-02 是边界 API 强化 |
| 控制条只有图标中心能点 | T51/T55 外扩 6px + 不透明 overlay | T51/T55 命中区通过；但控制条可见时底部/底角缩放被遮（A-P1-01）；320×180 溢出（A-P0-04） | **主问题已修，边界未修干净** |
| 实体视频无法播放 | T48 直连兜底 + T50 http-header-fields 修复 | T61 真实 Emby 4.9.1.90 已验 206 + Authorization；外挂字幕仍缺（A-P1-24）；错误处理仍差（A-P0-08） | **接口链路已通，需实机复测播放/切集/错误路径**，不能判定完全修好 |
| 首页缩放/滚动异常 | T47 缩放/滚动回归修复 | T59 复现 800/960 content 1022 vs viewport 546/706，横向滚动关闭（A-P0-03） | **未修干净（P0）** |
| 播放模式最小尺寸 | T52 放开 320×180 | T59 发现 320×180 控制条 394px/顶栏+控制条 184px 溢出（A-P0-04） | **未修干净（P0）** |

---

## 5. Requirements P0 覆盖矩阵

> 依据 [docs/requirements.md](../docs/requirements.md) 的 41 条 P0；状态：已满足 = 代码+容器测试/真实联调证据齐；部分满足 = 主路径可用但验收项未全过；未满足 = 有明确 P0 失败；未验证 = 只能在 Windows/GPU/实机验证。

| 需求 | 条目 | 状态 | 证据/说明 | 关联问题 |
|---|---|---|---|---|
| CON-01 | 服务器地址与连通性 | 已满足 | 登录页 + 假服务器 selftest + 真实 Emby 联调 | — |
| CON-02 | 用户名密码登录 | 已满足 | `AuthenticateByName` + 假服务器/真服务器验证 | — |
| CON-03 | 登录保持与退出 | 已满足 | TokenStore + 过期 token 回登录页 selftest；明文存储见 A-P1-17 | A-P1-17 |
| CON-04 | Emby/Jellyfin 兼容探测 | 部分满足 | Emby 4.9 已联调；Jellyfin 未实测，首次探测仍发 legacy 头 | P2-EM-06 |
| LIB-01 | 首页继续观看+最新 | 部分满足 | 真实 live-data 通过；800/960 溢出 | A-P0-03 |
| LIB-02 | 媒体库海报网格 | 部分满足 | 200 mock + 96 真实项通过；1000 项/55fps 未实测 | A-P0-03、性能待验 |
| LIB-03 | 详情页 | 已满足 | 真实详情/季集 live-data 通过 | — |
| LIB-04 | 图片加载与缓存 | 部分满足 | 缓存/回退链通过；图片 URL 带 token | A-P0-01 |
| PLY-01 | 直连播放 | 部分满足 | 真实 206+Authorization 通过；实机播放未闭环，错误处理不达标 | A-P0-08、A-P1-21 |
| PLY-02 | 硬解零回拷 | 未验证 | 配置正确，Windows GPU 数值待测 | A-P0-V1、A-P0-10 |
| PLY-03 | HDR10 tone-map | 未验证 | 配置正确，S1 实机待测 | A-P0-V1 |
| PLY-04 | DV P5/P8 tone-map | 未验证 | gpu-next/libdovi 依赖；无 DV 检测/提示 | A-P0-V2、A-P1-26 |
| PLY-05 | ASS 特效字幕 | 未验证 | 配置正确；Windows 渲染未测；外挂字幕缺失 | A-P1-24 |
| PLY-06 | 字幕/音轨切换 | 部分满足 | 音轨切换+回退通过；外挂字幕/轨道观察缺失 | A-P1-24、A-P1-27 |
| PLY-07 | 播放控制条 | 部分满足 | Qt overlay 可用；320×180 溢出；OSC 口径冲突 | A-P0-04、A-P0-D1 |
| PLY-08 | 键盘与鼠标 | 未满足 | N/P、双击全屏失败；键盘双转发风险 | A-P0-05、A-P0-06、A-P1-20 |
| PLY-09 | 上下集与队列 | 部分满足 | 按钮队列可用；N/P 键路径缺 | A-P0-05 |
| PLY-10 | 续播 | 未满足 | 无 >95% 重置/自动续播开关 | A-P0-07 |
| PLY-11 | 播放统计 | 部分满足 | statsRequested→toggleStats 已接；Windows 未测 | A-P0-V1 |
| PLY-12 | 播放错误处理 | 未满足 | 无错误码/重试/返回；重试超限 | A-P0-08 |
| PLY-13 | 全屏 | 部分满足 | F/Esc 链路通过；双击不进全屏、多屏/全屏边缘命中问题 | A-P0-06、A-P1-02、A-P1-23 |
| SYN-01 | 开始上报 | 部分满足 | 三接口已接；Start 早于 fileLoaded/失败幽灵会话 | A-P1-14 |
| SYN-02 | 进度上报 | 部分满足 | 10s timer + 事件即时上报；401 不停 timer；30 分钟真机未测 | A-P1-15 |
| SYN-03 | 停止上报 | 未满足 | 退出异步请求丢失 | A-P0-09 |
| WIN-01 | 无边框/最小尺寸 | 部分满足 | 800×480/320×180 已实现；底边缩放/320 UI 不可用 | A-P1-01、A-P0-04 |
| WIN-02 | 贴边/最大化/Snap | 部分满足 | 最大化/边缘命中已实现；按钮顶部 8px、全屏边缘 | A-P1-03、A-P1-02 |
| WIN-03 | 高 DPI/多显示器 | 未验证 | 仅 offscreen 1.5x 截图；Windows DPI/多屏未测 | — |
| WIN-04 | 窗口状态恢复 | 部分满足 | T27/selftest 通过；Windows 未复测 | P2-WIN-02 |
| SET-01 | 设置页 | 部分满足 | 服务器/播放/存储/关于齐全；缺 autoResume/硬解模式/音频直通 | A-P0-07、A-P0-10、A-P1-25 |
| SET-02 | 日志 | 未满足 | app.log 有脱敏；mpv.log 含 token | A-P0-02 |
| SET-03 | 关于与诊断 | 未满足 | 诊断包会带 mpv.log token；崩溃 dump 风险 | A-P0-02、A-P1-18 |
| PKG-01 | 安装包 | 部分满足 | CI zip/installer 通过；干净机安装/登录/播放未验 | — |
| PKG-02 | 卸载/升级 | 未验证 | 未在干净 VM 做升级/卸载 | — |
| PKG-03 | 崩溃日志 | 部分满足 | handler 已实现；minidump 含数据段 | A-P1-18 |
| NFR-01 | 启动/交互性能 | 未验证 | 容器 offscreen 数据不作为 Windows 结论 | — |
| NFR-02 | 播放性能/资源 | 未验证 | 需 Windows GPU + S1/S7 | A-P0-V1、A-P0-V2 |
| NFR-03 | 稳定性 | 未验证 | 8h soak/100 次切集未做 | — |
| NFR-04 | 兼容性 | 未满足 | 硬解失败无 auto-safe 提示；GPU/OS 矩阵未测 | A-P0-10 |
| NFR-05 | 安全 | 未满足 | 图片 URL token、mpv.log token、明文 QSettings、minidump | A-P0-01、A-P0-02、A-P1-17、A-P1-18 |
| NFR-06 | 国际化 | 已满足 | 简中界面、tr() 覆盖；英文为 P1 | — |
| NFR-07 | 构建可复现 | 已满足 | build-linux + Windows CI + installer 通过 | — |

**统计**：已满足 6 / 部分满足 18 / 未满足 8 / 未验证 9。未满足项全部有对应 P0/P1 条目，不存在“需求漏项”。

---

## 6. 批修计划（按文件/模块分组，避免冲突）

### 6.1 文件所有权矩阵（一个文件同时只有一个 owner）

| 文件/目录 | Owner | 说明 |
|---|---|---|
| app/src/windowframe.*, videohost.*, mpvplayer.*, mainwindow.*（controller）, main.cpp, crashhandler.* | build-verifier | 原生窗口/播放内核/控制器/崩溃 |
| app/src/ui/**（homeview、playerpage、settingspage、logindialog、ui/mainwindow、sidebar、titlebar、style、demo_main、embythumbnailcache、tokenstore 等） | ui-builder | UI/布局/控制条/登录/缓存/tokenstore |
| app/src/emby/** | emby-api | API 客户端、DeviceProfile、类型、测试 |
| docs/**、README.md、installer/**、app/scripts/**、.github/** | release-engineer | 验收口径、发布说明、打包/CI 文档 |
| reports/full-audit.md（本文件） | planner | 汇总；不改产品代码 |

**协作规则：**

- 同一文件同一时间只允许一个 owner 修改；跨文件问题由 owner 之间用接口/信号约定，不互相改对方文件。
- `app/src/ui/embythumbnailcache.cpp` 归 ui-builder（文件在 ui/），A-P0-01 的安全语义由 emby-api 评审；`tokenstore.*` 归 ui-builder，A-P1-17 的 DPAPI 实现由 emby-api 提供安全评审。
- 接口先定后并行：`player/autoResume` QSettings key、`MpvPlayer` 的 session/header 状态、`PlayerController` 的错误/停止信号，先由 build-verifier 定，ui-builder 再接 UI。
- 合并顺序：B1 三条车道可并行；B2 也按车道并行；B3 P2 最后。`demo_main.cpp` 的测试断言在 B1 期间由 ui-builder 统一改，避免与 emby-api 抢文件。

### 6.2 批次总览

| 批次 | 内容 | Owner/车道 | 预估 | 依赖 |
|---|---|---|---|---|
| **B1** | P0 修复（3 条并行车道 + 决策/验证） | build-verifier / ui-builder / lead+release-engineer / video-analyst | 5–6 人日 | 无 |
| **B2** | P1 修复（3 条并行车道 + 文档） | build-verifier / ui-builder / emby-api / release-engineer | 9–11 人日 | B1 合入后 |
| **B3** | P2 清理（按模块） | build-verifier / ui-builder / emby-api | 4–6 人日 | B2 后，0.1.2 |
| **B4** | Windows 实机回归与发布判定 | video-analyst + 用户 + lead | 1–2 天 | B1/B2 构建产物 |

> 估算为纯编码+容器自测，不含 Windows 等待、review、返工。3 条车道并行时 B1 预计 2–3 个工作日、B2 预计 3–4 个工作日。

### 6.3 B1：P0 修复（发布阻断）

#### B1-A 播放内核/控制器（build-verifier）

- **任务**：A-P0-02、A-P0-05、A-P0-06、A-P0-07（controller 部分）、A-P0-08、A-P0-09、A-P0-10。
- **文件**：app/src/mpvplayer.*、app/src/videohost.*、app/src/mainwindow.*、app/src/main.cpp、app/src/crashhandler.*。
- **验收标准**：
  - 日志/诊断包 grep 无 token；切换剧集/重新登录后仍无。
  - N/P 切集、双击全屏在 Windows 实机通过；本地文件 N/P 不回归。
  - 98% 条目从头；关闭 autoResume 从头；退出后 Dashboard 位置误差 ≤2 秒。
  - 5 类播放错误有错误码 + 重试/返回，重试 ≤2；失败无幽灵会话。
  - hwdec-current 正常为 d3d11va；失败时 auto-safe/软解 + 提示。
- **必须补的回归**：新增 mpv 初始化失败/日志脱敏/会话收尾的 offscreen 或本地 libmpv 测试；全量 UI selftest 仍 0 FAIL；真实 Emby Dashboard 复测。
- **预估**：3.25 人日。

#### B1-B UI（ui-builder）

- **任务**：A-P0-01（图片 token）、A-P0-03（首页溢出）、A-P0-04（320/640 控制条）、A-P0-07（设置开关）、A-P0-10（设置硬解模式，可选）。
- **文件**：app/src/ui/**。
- **验收标准**：
  - 图片 URL 无 api_key，功能不回归；demo_main 断言改为“无 token”。
  - 800/960/1280 三档：外层 content 宽度=viewport、横向 scroll maximum=0、无裁切。
  - 320×180 或 640×360：按钮 rect 全在控制条内、顶栏/控制条不重叠。
  - 设置 autoResume/硬解模式持久化。
- **必须补的回归**：--selftest 增加布局断言（目标 ≥200 PASS）；截图 06-home-960x600、播放器 320/640、设置 800 各一张人工走查。
- **预估**：2 人日。

#### B1-C 口径与版本（lead + release-engineer）

- **任务**：A-P0-D1 决策记录（建议保留 Qt 控制条）；更新 requirements PLY-07、windows-acceptance O 组、README/decisions；CMake 版本 0.1.0→0.1.1 在 B4 前完成。
- **验收**：文档不再要求 mpv OSC；OSC 明确为 EMBY_PC_MPV_OSC=1 调试选项；文档之间不互相矛盾。
- **预估**：0.25 人日。

#### B1-D Windows P0 验证（video-analyst + 用户）

- **任务**：A-P0-V1（S1 HDR10）、A-P0-V2（S2/S3 DV P5/P8）+ §4 用户已报项复测。
- **验收**：S1 不发灰；S2/S3 不绿/紫；hwdec-current=d3d11va、current-vo=gpu-next；窗口/全屏/边缘截图；结果写 docs/test-reports/v011-p0-<date>.md。
- **注意**：无 Windows 机器则 v0.1.1 只能发 rc，不能发 stable。

### 6.4 B2：P1 修复（用户可感知项优先）

#### B2-A 原生/控制器（build-verifier）

- **任务**：A-P1-02、A-P1-03、A-P1-04、A-P1-05、A-P1-06、A-P1-13、A-P1-14、A-P1-15、A-P1-18、A-P1-20、A-P1-21、A-P1-22、A-P1-24、A-P1-26、A-P1-27。
- **文件**：windowframe/videohost/mpvplayer/controller/crashhandler。
- **验收**：全屏/最大化按钮/底边缩放正常；会话失败/401/退出清理干净；键盘不双转发（Windows 实测）；慢起播不误判；音量重启保留；外挂字幕可见；track-list 切换后刷新；DV 有提示。
- **回归**：Linux build；UI selftest 0 FAIL；Windows 手工 8 项；真实 Emby 切集/外挂字幕/401。
- **预估**：6.5 人日。

#### B2-B UI（ui-builder）

- **任务**：A-P1-01、A-P1-07、A-P1-08、A-P1-09、A-P1-10、A-P1-11、A-P1-12、A-P1-17、A-P1-23、A-P1-25（UI 部分）。
- **文件**：app/src/ui/**。
- **验收**：控制条可见时底边可缩放；首页错误/加载态可重试；800 设置页不溢出；登录框 520/自适应 + 密码显隐；DPAPI 存储可迁移；多屏全屏在鼠标屏；a11y 名称/焦点。
- **回归**：selftest 新增错误态/800 设置/键盘卡片/焦点断言；截图走查。
- **预估**：4.5 人日。

#### B2-C Emby API（emby-api）

- **任务**：A-P1-16、A-P1-19（client 侧）、P2-EM-01/02/05/06/07/08 中与客户端相关的部分。
- **文件**：app/src/emby/**。
- **验收**：跨主机 URL 不带 token；切换 profile abort/代际；token 别名清理；Jellyfin 首探不带 legacy token；DeviceProfile 覆盖 PGS/SUP/常见容器；幂等 GET 重试。
- **回归**：embyclient_test 新增 same-origin/legacy/migration/retry 用例；ctest 全绿；live_probe 对真实服务器通过。
- **预估**：1.5 人日。

#### B2-D 文档/验收（release-engineer）

- **任务**：更新 docs/windows-acceptance.md（图片 URL/mpv.log token 检查、800/320 场景、P0 清单）；README 状态；release notes。
- **验收**：验收步骤可直接执行；每个 P0 有对应测试步骤与通过标准。
- **预估**：0.5 人日。

### 6.5 B3：P2 清理（0.1.2，不阻塞 v0.1.1）

| 子批 | Owner | 内容 | 预估 |
|---|---|---|---|
| B3-A | build-verifier | P2-CORE-01..14 + P2-WIN-01..04 | 2.5 人日 |
| B3-B | ui-builder | P2-UI-01..15 | 2.5 人日 |
| B3-C | emby-api | P2-EM-03/04 + B2-C 未覆盖的 P2-EM 项 | 1.5 人日 |

---

## 7. 回归测试计划

### 7.1 容器内自动化（每个 PR/批次必须跑）

| # | 命令/入口 | 通过标准 |
|---|---|---|
| R1 | `./app/scripts/build-linux.sh` | 配置+编译成功；无新增错误 |
| R2 | `cmake -S app/src/ui -B /tmp/emby-ui-build -DEMBY_UI_BUILD_DEMO=ON && cmake --build /tmp/emby-ui-build -j && QT_QPA_PLATFORM=offscreen /tmp/emby-ui-build/emby-ui-demo --selftest` | 基线 190 PASS / 0 FAIL；B1 后 ≥200 PASS（新增布局/设置/错误态），0 FAIL |
| R3 | `cmake -S app/src/emby -B /tmp/emby-build -DEMBY_BUILD_TESTS=ON && cmake --build /tmp/emby-build -j && ctest --test-dir /tmp/emby-build --output-on-failure` | 现有 95 checks 全过 + 新增 token/同源/401/重试用例全过 |
| R4 | `QT_QPA_PLATFORM=offscreen ... --screenshot app/src/ui/preview` | 生成 14+ 张截图；人工检查 800/960 首页、320/640 播放器、设置页 |
| R5 | 敏感词扫描：启动真实 Emby 播放 30s→退出→grep app.log/mpv.log 的 api_key/Authorization/Token/X-Emby-Token | 0 命中（或仅脱敏占位） |
| R6 | `.github/workflows/windows-build.yml` | Windows 构建、emby tests、zip、installer、VERSIONINFO 全绿；产物可下载 |
| R7 | `git diff --check` / 代码审查 | 无空白错误；无 token/内网地址入库 |

### 7.2 Windows 实机手工回归（B1/B2/B4 必做）

| # | 场景 | 预期 | 关联 |
|---|---|---|---|
| M1 | 播放模式拖到 320×180（或 640×360） | 控制条/顶栏不重叠；按钮可点；边缘可缩放 | A-P0-04、A-P1-01 |
| M2 | 800×480 / 960×600 主页与设置页 | 首页卡片不裁切、无横向滚动；设置媒体库行不溢出 | A-P0-03、A-P1-10 |
| M3 | 剧集播放按 N/P；双击画面；按 F/Esc | N/P 切集；双击进出全屏；F/Esc 正常；空格只切换一次 | A-P0-05/06、A-P1-20 |
| M4 | 播放 15s 关闭；查看 Emby Dashboard | 继续观看位置误差 ≤2s；无幽灵会话 | A-P0-09、A-P1-14 |
| M5 | 已看 98% 条目 / 关闭自动续播 | 从头播放 | A-P0-07 |
| M6 | 断网/删文件/改错地址后播放 | 3s 内错误码 + 重试/返回 | A-P0-08 |
| M7 | 正常 GPU / 人为禁用硬解 | hwdec-current=d3d11va / 自动回退 + 提示 | A-P0-10、A-P0-V1 |
| M8 | S1 HDR10、S2 DV P5、S3 DV P8 | 不发灰/不绿/不紫；与 mpv CLI 参考一致 | A-P0-V1/V2 |
| M9 | 控制条可见时拖底边/底角；最大化/Snap；全屏边缘 | 底边可缩放；无 1px 缝；全屏无 resize cursor | A-P1-01/02/03 |
| M10 | Emby 外挂字幕条目 | 字幕菜单出现外挂轨，能加载 | A-P1-24 |
| M11 | 图片请求抓包 | URL 无 api_key/token；Authorization 头存在 | A-P0-01 |
| M12 | 退出后检查 mpv.log/诊断包 | 无 token；日志有轮转 | A-P0-02 |
| M13 | 干净 Win10/Win11 安装→登录→播放→卸载/升级 | 全流程通过；VERSIONINFO 0.1.1 | PKG-01/02 |
| M14 | 多显示器：窗口 A 屏、鼠标 B 屏按 F | 在 B 屏全屏；退出回原屏 | A-P1-23 |
| M15 | 8h 播放 + 100 次切集 | 无崩溃；RSS 增长 ≤10%/100MB | NFR-03 |

### 7.3 每个 P0 的完成定义（DoD）

1. 代码修复已合入；有对应单测/offscreen 断言或明确的 Windows 手工证据。
2. 日志/诊断包敏感词扫描通过（安全类）。
3. 受影响的 requirements 条目更新（若行为/口径变化）。
4. 回归命令 R1–R7 全绿；新引入 P0 一票否决。
5. 用户已报项必须附实机截图/日志；不能只凭代码推断关闭。

---

## 8. v0.1.1 发布判定

### 8.1 当前判定：**不发布**

当前 0.1.0 构建存在 10 个代码 P0（含 2 个 token 泄露）和 2 个用户已报 P0 回归（首页缩放、320×180 控制条），且 P0-V1/V2 未在 Windows 验证。直接发 v0.1.1 会把已知安全和体验缺陷带给用户。

### 8.2 v0.1.1 发布门槛（全部满足才可发）

| 门槛 | 内容 |
|---|---|
| G1 P0 全修 | A-P0-01..10 全部关闭，DoD §7.3 通过 |
| G2 决策落地 | A-P0-D1 已由 lead 决策，requirements/acceptance/README 同步；OSC/Qt 控制条不再矛盾 |
| G3 实机验证 | A-P0-V1（HDR10）、A-P0-V2（DV P5/P8）在 Windows 实机 PASS；失败项已修复并复测 |
| G4 用户已报复测 | §4 的 8 项全部有实机 PASS 证据，尤其首页缩放/播放模式最小尺寸/控制条点击/实体视频 |
| G5 回归 | R1–R7 全绿；UI selftest ≥200 PASS/0 FAIL；Emby ctest 全绿；Windows CI 构建+安装包成功 |
| G6 干净机 | 干净 Win10 22H2、Win11 23H2 安装→登录→播放→升级/卸载通过 |
| G7 安全 | app.log/mpv.log/诊断包/崩溃 dump 中 token 扫描 0 命中；图片/流 URL 无 token |
| G8 版本 | CMake/安装包/VERSIONINFO 均为 0.1.1；release notes 列出 P0 修复与已知 P1/P2 |

### 8.3 建议节奏

- **B1（P0）合并后**：先发 **v0.1.1-rc1** 给内测/用户 Windows 机器，做 A-P0-V1/V2 与 §4 复测。
- **B2（P1 用户可感知项）合并后**：发 **v0.1.1-rc2**，跑 R1–R7 + 干净机 M13。
- **G1–G8 全绿**：发 **v0.1.1 stable**。
- **P2**：0.1.2 起清理；不阻塞 v0.1.1。
- 若 Windows 机器暂时不可用：只能发 **v0.1.1-rc**（明确标注未实机验证），不得把 stable 发给用户。

### 8.4 版本与变更记录

- 版本号：app/CMakeLists.txt 的 project(emby-pc VERSION 0.1.0) → 0.1.1；安装包/VERSIONINFO 随之更新。
- release notes 必含：2 个安全修复（token 不进 URL/日志）、首页/小窗布局、N/P/双击/续播/错误处理/退出上报/硬解降级、用户已报项复测结果、已知 P1/P2 清单。
- 打 tag 前确认 git status 干净、无凭据/日志文件入库。

---

## 9. 附录：基线证据、限制与待决事项

### 9.1 当前基线证据

- commit：`a28e44b`（T56/T57 已合入；工作树无未提交产品代码改动）。
- UI selftest：190 PASS / 0 FAIL（2026-11-06 本容器实测）。
- Emby 单测：ALL TESTS PASSED（T60 记录 95 checks；本容器复跑通过）。
- 真实 Emby：4.9.1.90 直连 206 + Authorization 头（reports/emby-live-validation.md）。
- 容器限制：无 Windows/GPU/libmpv-2.dll，不能验证 --wid 嵌入、D3D11VA、HDR/DV、ASS 实际渲染、DWM 边框、安装包实机行为。

### 9.2 待决事项（需要 lead/用户）

1. **A-P0-D1**：确认保留 Qt 控制条、OSC 仅调试选项（推荐）；否则需排 1d+ 改造。
2. **A-P1-20**：键盘转发是否双触发需 Windows 实机 5 分钟实测（空格/F/I 是否触发两次）；测试方法见 T61 §6.1。
3. **A-P0-04**：320×180 保留 compact 布局，还是把播放最小尺寸提高到 640×360（推荐后者，工作量/风险低）。
4. **A-P1-17**：是否本版做 DPAPI（1d）；若不做，需在 release notes 标注 token 本地明文风险。
5. **Windows 机器**：B1-D/B4 需要可用的 Win10/Win11 实机与 S1~S3 素材；没有则 v0.1.1 只能 rc。
6. **P1 范围**：v0.1.1 是否只修 ★ 项（约 14 项）还是全部 27 项；本报告建议至少修完 ★ 与用户已报项，其余可移 0.1.2。

### 9.3 本报告的边界

- 只做汇总/计划，不改产品代码；所有 file:line 来自 T58–T61（基线 a28e44b），落地时以最新代码 rebase 后再核对。
- 严重度是“发布阻塞视角”的工程判断，不是安全评级；A-P0-01/02 需按安全事件处理（清理旧日志、轮换 token）。
- 本报告不能替代 Windows 实机验收；所有“未验证”项必须在 Windows 上闭环后才能关闭。


