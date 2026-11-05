# T21 首页「继续观看」为空排查

> 任务：task-21 · 负责人：build-verifier · 日期：2026-11-05
> 用户基线：run 41872144136（T19）；反馈「进度同步正常，但程序首页继续观看为空」；同一账号在 Emby 官方客户端继续观看正常。
> 范围：`app/src/ui/librarydata.cpp`、`app/src/ui/homeview.cpp`、`app/src/mainwindow.cpp/.h`、本报告。未改 `app/src/emby/`；T20 的 librarydata 海报改动保留。

## 1. 结论

- **直连和反向代理都无法复现「接口返回 0」**：测试账号按客户端完全相同参数请求 `GET /Users/{userId}/Items/Resume`，直连 8096 与 `https://j.<host>:222`（Caddy 反代，上游 8091）均 HTTP 200、TotalRecordCount=1；用 `LibraryData::fetchHome` + 真实 `ui::MainWindow` 跑同样请求，首页 resumeCount=1、dataLoaded=true。
- 因此 **server 接口、代理、EmbyClient、ItemQueryResult/UserData 解析都正常**。T24 进一步定位：`HomeView::rebuildResume()` 在 layout 激活前调 `adjustSize()`，content 尺寸保持 0x0，卡片虽创建但高度被压成 0——这是 UI 空白的直接根因；详见第 7 节。T21 的首页刷新/Stopped 位置修复仍保留（它们解决播放后数据不更新的问题）。
- 已修 4 个客户端隐患：
  1. **播放结束/退出播放后不刷新首页**：从首页进入播放再返回时，首页仍显示播放前的空 Resume；现在 Stopped 返回后和回到首页时都会重新 fetchHome。
  2. **Stopped 可能上报 0 位置**：mpv stop 后 position 可能归零；现在 stop 前记录最后位置，Stopped 用该值，避免服务器把条目从 Continue Watching 清掉。
  3. **最近添加失败会连带丢弃继续观看**：fetchHome 原来共用一个 error，UI 在 error 时直接 return；现在 resume/recent 错误分开，resume 非空时 recent 失败不再清空继续观看。
  4. **Resume 响应缺 PlayedPercentage 时进度为 0**：现在用 PlaybackPositionTicks/RunTimeTicks 回算；空状态文案也改为「暂无继续观看（可能已看完，或该账号没有未看完条目）」。
- 已补日志：resume 请求/响应会写 userId、HTTP 状态、TotalRecordCount、Items 数、解析后条数、错误串、首条 UserData 字段（不含 token）。

## 2. 真实服务器复现

### 2.1 直连 Emby（内网服务器:8096，测试账号；地址已脱敏）

按客户端完全相同的 query（Limit=12、Fields=MediaSources,MediaStreams,Overview,UserData,ProductionYear,PrimaryImageAspectRatio,DateCreated,OfficialRating,CommunityRating,Genres,RunTimeTicks,Container,Path、MediaTypes=Video、EnableImageTypes=Primary,Backdrop,Thumb、ImageTypeLimit=1、EnableUserData=true）：

```text
HTTP 200
TotalRecordCount = 1
Items[0] = { Id=1056815, Name=边境奇袭, Type=Movie, MediaType=Video,
             UserData.Played=false, UserData.PlaybackPositionTicks=9599996615,
             UserData.PlayedPercentage=18.7128, RunTimeTicks=51301760000 }
```

对比：**不带客户端 Fields/MediaTypes 的简版请求返回 TotalRecordCount=0**。说明该服务器/版本的 Resume 对 query 参数敏感；客户端目前发送的是完整参数，直连可返回 1。若代理层改写/丢弃 query，可能表现为空，需要日志/代理配置确认。

### 2.2 用真实 UI 链路复现

临时程序（不入库）用真实 `EmbyClient` 登录 测试账号（直连 8096 和 `https://j.<host>:222` 各跑一次，结果一致），把 profile 注入 `ui::MainWindow`（requireAuthentication=true），等待首页 dataLoaded：

```text
resume request: userId "276a...175b" limit 12
resume response: userId "276a...175b" httpStatus 200 totalRecordCount 1 items 1 error false message ""
resume parsed: 1 error ""
resume first: id "1056815" name "边境奇袭" type "Movie" played false positionTicks 9599996615 playedPercentage 18.7128 runTimeTicks 51301760000
dataLoaded true ""
loaded true error "" resumeCount 1
```

结论：**真实 UI 能渲染出继续观看**；因此用户遇到的空态不是「解析/HomeView 一定坏了」，而是环境/时序。

## 3. 客户端修复

### 3.1 Resume 请求日志

`LibraryData::fetchHome` 增加：

- 请求：`resume request: userId ... limit ...`
- 响应：`resume response: userId ... httpStatus ... totalRecordCount ... items ... error ... message ...`
- 解析：`resume parsed: N error ...`
- 首条：id/name/type/played/positionTicks/playedPercentage/runTimeTicks
- recent 错误：`home recent response error: httpStatus ... message ...`

不记录 accessToken；error message 截断 200 字符。新包 app.log 可直接区分：

- `httpStatus 200 totalRecordCount 0`：服务器确实返回 0（看账号/代理/已看完）。
- `httpStatus 200 totalRecordCount >0 items 0`：解析/响应体异常。
- `httpStatus 401/404/5xx` / `httpStatus 0`：请求失败/代理/证书。
- `resume parsed >0` 但页面空：UI 刷新时序。

### 3.2 首页刷新

`PlayerController` 新增 `scheduleHomeRefresh(delayMs)`：

- 离开播放页 / 返回按钮 / stack currentChanged 后延迟 600ms，如果当前 Stack 是 Home，调用 `MainWindow::navigateTo(Home)` 重新 fetchHome。
- `reportPlaybackStopped` HTTP 回调返回后立即再调度一次刷新，确保服务器已写入 Resume。

### 3.3 Stopped 位置

- `positionChanged` 持续记录 `m_lastKnownPositionSeconds`。
- stack 离开播放页时，在 `m_player->stop()` **之前**再取一次位置。
- `reportPlaybackStopped` 用最后已知位置；报告日志 `playback stopped report: item ... positionSeconds ...`。

### 3.4 错误隔离 / 进度回算 / 空文案

- `fetchHome` 的 `resumeError` / `recentError` 分离：resume 非空时 recent 失败不再清空继续观看。
- `toMediaItem`/`toEpisode`：PlayedPercentage<=0 且有 PlaybackPositionTicks/RunTimeTicks 时回算百分比。
- HomeView 空状态改为「暂无继续观看（可能已看完，或该账号没有未看完条目）」。

## 4. 验证

### 4.1 构建

```bash
cd /root/github/emby-pc/app
./scripts/build-linux.sh
# ==> OK: app/build-linux/emby-pc
```

额外 `-Wall -Wextra` 构建通过，无新增 warning。

### 4.2 真实服务器

- 直连 Emby：客户端同参 query 返回 HTTP 200 / total=1。
- 真实 `ui::MainWindow` + 真实账号：resumeCount=1、dataLoaded=true（见 2.2 日志）。
- 代理 `https://j.<host>:222`：容器可通过 Caddy 访问；同参 query 返回 HTTP 200 / total=1，真实 UI 探针 resumeCount=1。

### 4.3 Windows CI

- run `41952931196`（commit `81c2751`，含 T21+T22）全绿；MSVC 2022 x64 的 Build (Release) 通过；artifact `emby-pc-win64` id `13382555893`（72,221,356 bytes）。

### 4.4 自测

- 现有 UI 自测/构建：T20 selftest 105 PASS、live-data PASS、app 全量构建通过（ui-builder 结果）。
- 本任务真实 UI 探针：真实服务器 + 真实 `ui::MainWindow` → `resumeCount=1`、`dataLoaded=true`（见 2.2）。
- HomeView 独立测试：空列表 → `emptyCount=0`、空文案「暂无继续观看（可能已看完，或该账号没有未看完条目）」；注入 1 条 → `oneCount=1`、`oneCards=1`。
- 代理/真实客户端播放后刷新需用户实机验证。

## 5. 局限与下一步

- 用户代理 `https://j.<host>:222` 已在容器通过 Caddy 实测，同参 Resume 返回 1；新包仍需实机 app.log 观察客户端是否请求到代理、以及播放后刷新是否生效。
- 若新包仍为空：请导出诊断（帮助 → 导出诊断信息）或把 app.log 中以下行发回：
  `resume request`、`resume response`、`resume parsed`、`home recent response error`、`playback stopped report`。
  - `totalRecordCount 0` → 服务器/代理返回 0，需查代理 query/账号；
  - `totalRecordCount 1` 但 UI 空 → 时序/前端刷新；
  - 401/404/5xx/0 → 鉴权/代理/证书。
- 建议实机步骤：播放某条目到 ~50% → 返回首页（或 Esc 退全屏再返回）→ 观察首页 Continue Watching；再重启程序确认 Resume 持久化。

## 6. 改动文件

| 文件 | 改动 |
|---|---|
| `app/src/ui/librarydata.cpp` | resume 请求/响应/解析/错误日志；resume/recent 错误分离；PlayedPercentage 回算 |
| `app/src/ui/homeview.cpp` | 空状态文案 |
| `app/src/mainwindow.cpp/.h` | 最后已知位置；Stopped 用最后位置；首页刷新调度 |
| `reports/resume-gap.md` | 本报告 |
## 7. T24 追加：UI 卡片 0 高度根因（2026-11-05）

### 7.1 实机证据

- 用户 app.log（run 41952931196）：resume request 200、totalRecordCount=63、items=12、parsed=12、首条 Episode positionTicks=1.6e9/10.5%，但首页看不到卡片。
- 说明 LibraryData -> MainWindow 数据已经拿到，问题在 HomeView 展示。

### 7.2 根因

HomeView::rebuildResume() 原来在添加卡片后直接 m_resumeContent->adjustSize()。此时横向 layout 尚未 activate，sizeHint() 还是 0；QScrollArea 的 content widget 保持 QSize(0,0)，卡片虽然创建，但布局分配高度为 0（实测卡片 QRect(0,0 236x0)、hidden=true），所以用户看到空的「继续观看」区域。

静态 12 条假条目的 offscreen 复现：

```text
HomeView::rebuildResume: items 12 cards 0
HomeView::rebuildResume: cards 12 emptyVisible false content QSize(0, 0)
HomeView::rebuildResume: firstCard QRect(0,0 236x30) visible false hidden true
```

### 7.3 修复

- rebuildResume() 末尾：
  1. m_resumeLayout->activate() 强制布局计算；
  2. 用 m_resumeLayout->sizeHint() 设置 m_resumeContent 的固定尺寸（宽度为卡片总宽，高度 190）；
  3. 再用 QTimer::singleShot(0, ...) 在下一轮事件循环补一次尺寸，覆盖第一次 show/layout 前 sizeHint 仍为 0 的情况。
- 增加 T24 诊断日志：
  - LibraryData::fetchHome：recent 请求/响应/解析/finish 等待与完成（resumeDone/recentDone/条数/错误）；
  - MainWindow::loadRealHome callback：generation/current、resume/recent 条数、error、homeVisible、stackIndex；
  - HomeView::setResumeItems/rebuildResume/resizeResumeContent：items/cards/emptyVisible/content size/scroll viewport/first card geometry；
  - diagnostics 增加 HomeView: resumeCount/cards/loading/error、sectionVisible/section geometry/scroll/viewport/content/emptyVisible/emptyText、firstCardVisible/geometry。
- 未修改 embythumbnailcache/thumbnailcache（T23 范围）；没有对用户真实服务器做播放/重复请求，测试只用静态 12 条假数据。

### 7.4 验证

静态 12 条条目 + offscreen HomeView：

```text
HomeView::resizeResumeContent: hint QSize(2986, 190) content QSize(2986, 190)
cards 12
firstCard geometry QRect(0,0 236x190), visible true
diagnostic content=2986x190, emptyVisible=0
exit=0
```

- LibraryData::fetchHome 的 recent/finish 日志已验证：真实探针一次返回 recent=24、resume=2，home finish 正常触发；用户数据 12 条时同一路径。
- 构建：Linux Debug 通过；待 Windows CI 重新编译。
- 实机验证：首页继续观看应出现 12 张横向卡片（首屏至少 3 张），鼠标横向滚动可看其余；若仍空白，导出 diagnostics 会包含 HomeView: resumeCount=... cards=... content=... emptyVisible=...，可直接判断是数据为空还是布局尺寸异常。
