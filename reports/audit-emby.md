# Emby 集成 / 鉴权 / 会话 / 错误处理审查（T60）

> 任务：task-60（只审查，不改代码）
> 基线：`a28e44b`（T56/T57 已合入）
> 审查日期：2026-11-06
> 主范围：`app/src/emby/*`、`app/src/mainwindow.cpp` 的 Emby 会话/PlaybackInfo/进度上报逻辑
> 关联检查：`app/src/mpvplayer.*`、`app/src/ui/{embythumbnailcache,tokenstore,mainwindow,librarydata}.*`、`app/src/crashhandler.cpp`
> 只读审查：未修改任何产品代码；行号基于 `a28e44b`。

## 结论

- **P0：2 项**
- **P1：8 项**
- **P2：10 项**

P0 两项都是 **token 明文泄露**，且都被现有验收标准明确判为 P0：

1. 图片请求把 access token 拼进 URL（`api_key`）。
2. mpv 运行中更新 `http-header-fields` 时，token 被写入 `mpv.log`；`mpv.log` 会被“导出诊断信息”打包。

> 说明：`QSettings` 明文存 token 不列为 P0，因为 `reports/gaps.md:41` 把当前阶段的 P0 安全边界定义为“不进日志、不拼进 URL”；但发布前仍需 DPAPI/凭据管理器（见 P1-6）。

---

## P0（阻塞发布，建议本批优先修）

### P0-1 图片请求把 access token 放进 URL 查询参数 `api_key`

- **文件行**：`app/src/ui/embythumbnailcache.cpp:436-446`
- **现象**：每个海报/背景图请求先构造 token-free 的 key，却又把当前 access token 追加到实际请求 URL：
  ```cpp
  requestQuery.addQueryItem(QStringLiteral("api_key"),
                            m_client->profile().accessToken);
  ```
  同时仍设置 `Authorization` 头。
- **触发条件**：登录后任何真实图片加载（首页/媒体库/详情/播放页 poster 请求）。
- **影响**：
  - token 出现在 URL 中，会被 Emby/Caddy/反向代理/抓包/网络日志记录；
  - 违反 `docs/requirements.md:82`（PLY-01：token 只在请求头、URL 中不出现）和 `docs/requirements.md:137`（NFR-05）；
  - 与 `app/src/emby/README.md:84`、`app/src/ui/README.md` 的“URL 不带 token”说明直接矛盾。
- **现有测试反而固化了这个行为**：`app/src/ui/demo_main.cpp:1794-1807` 的断言是“图片请求使用 api_key 查询参数”，修复 P0 时必须同步改测试；否则测试会阻止修复。
- **修复建议**：图片请求只发 `Authorization`（必要时加 `X-Emby-Token` 头），完全删除 `api_key` query；缓存 key 继续使用 token-free 的 `imageUrl()`；同步修改 `demo_main` 断言、README 和 `docs/windows-acceptance.md` 的 L-05 说明。
- **预估**：0.25 天（含测试断言修改）。

### P0-2 `mpv.log` 记录完整 `http-header-fields`，导出诊断即泄露 token

- **文件行**：
  - `app/src/mpvplayer.cpp:171-177`：`setHttpHeaderFields()` 调用 `mpv_set_property_string("http-header-fields", ...)`；
  - `app/src/mpvplayer.cpp:284-287`：`log-file` + `msg-level=all=v`；
  - `app/src/mainwindow.cpp:725-727`：mpv 已初始化时每次 Emby 播放都会调用上面的 setter；
  - `app/src/mainwindow.cpp:1659-1666`：`exportDiagnostics()` 把 `mpv.log` 复制进诊断包并让用户回传。
- **现象（本地 libmpv 0.40 复现）**：开启 `log-file` 后调用 `setHttpHeaderFields()`，mpv 会写入类似：
  ```text
  [v][cplayer] Set property: http-header-fields="Authorization: MediaBrowser ... Token=\"<redacted>\"" -> 1
  ```
  `mpv.log` 中可直接检索到完整 token；把 `msg-level` 降到 `all=info/warn/error` 仍不能消除该条 setter 日志（本地实测），所以不能只靠降日志级别解决。
- **触发条件**：mpv 已经初始化后开始播放任意 Emby 条目（切换集/电影/继续观看都走 runtime setter）。首次初始化时 header 在 `log-file` 之前设置，通常不落盘；但后续 runtime 更新一定落盘。
- **影响**：用户按验收流程导出 `app.log/mpv.log` 时，token 明文进入压缩包/工单；`docs/windows-acceptance.md:99` 明确规定 L-05 出现真实 token 即 P0 安全缺陷。
- **修复建议**（任选其一，推荐 1）：
  1. 不要再在 log-file 激活后调用 token setter；token 变化时重新 `initialize()/reinitialize()`，在 `log-file` 之前设置 `http-header-fields`；退出登录时用空 header 重初始化/清理；
  2. 设置 token 前临时关闭/切换 `log-file`，设置完再恢复，并验证日志中无 token；
  3. 在导出/写盘前对 `mpv.log` 做脱敏（但实时日志目录仍可能被直接查看，不能单独作为唯一方案）。
  同时增加回归测试：本地 mpv 用含 token 的 header，断言 `mpv.log` 不含 token；并清理/轮转已有含 token 的旧日志。
- **预估**：0.5 天（含 libmpv 回归测试）。

---

## P1（建议本批一起修，影响会话/安全/可诊断性）

### P1-1 退出登录/本地播放后，mpv 仍保留旧 Authorization header

- **文件行**：`app/src/mainwindow.cpp:619-628`（`loginStateChanged(false)` 只 stop，不清理 header）；`app/src/mainwindow.cpp:680-718`（本地播放 `ensurePlayer()` 不传 header）；`app/src/mainwindow.cpp:720-742`（只有 header 非空才更新）。
- **触发条件**：登录 Emby 播放 → 退出登录 → 打开本地文件；或退出登录后通过“所有文件”打开含远程 URL 的 .strm/播放列表。
- **影响**：旧 token 继续留在 mpv 的 `http-header-fields` 中，可能被发送给后续 HTTP 媒体 URL；也会进入进程内存/崩溃转储，扩大 P0-2 的暴露面。
- **修复建议**：logout 时显式清空 header（空字段 setter 或重初始化）；本地播放前确保 header 已清空；把“当前 header 状态”纳入 `MpvPlayer` 状态管理。
- **预估**：0.25 天。

### P1-2 播放失败/Start 上报失败后仍保持“正在上报”状态，产生幽灵会话

- **文件行**：`app/src/mainwindow.cpp:1146-1180`（`reportPlaybackStart()` 在 `fileLoaded` 前调用）；`app/src/mainwindow.cpp:1236-1267`（`handlePlaybackFailure()` 只弹窗/日志，不 `reportPlaybackStopped()`、不停 `m_progressTimer`）；`app/src/mainwindow.cpp:1689-1697`（`m_reporting=true` 先于 Start 请求成功）。
- **触发条件**：所有候选 URL 都失败（8s watchdog×候选数）、mpv 报错、或 `/Sessions/Playing` 请求失败。
- **影响**：Emby 服务端留下“正在播放”的幽灵会话；每 10s 继续发 Progress，甚至播放已结束仍显示在 Dashboard；停止位置没有写入。
- **修复建议**：失败/Start 错误统一走“结束会话”清理：停 progress timer、清 `m_reporting`、在可发送时补发 Stopped，再提示用户；Start 失败时不要先启动 timer。
- **预估**：0.5 天。

### P1-3 播放中收到 401/403 只弹登录框，不停止进度 timer/不清理旧会话

- **文件行**：`app/src/mainwindow.cpp:1700-1721`（Progress 错误回调）；`app/src/mainwindow.cpp:1785-1792`（`handleApiError` 仅 `requestLogin()`）。
- **触发条件**：token 在播放期间被服务端撤销/过期，或用户在其他设备注销。
- **影响**：progress timer 每 10s 继续发 401；登录框可能反复被 raise；旧条目的 Progress 用新登录/旧 token 继续尝试；Emby 端会话状态混乱。
- **修复建议**：抽出 `handleSessionExpired()`：停 timer、尽力发 Stopped、清 `m_reporting/m_embyItemId`、清 token store、更新登录态，再打开登录框；多个 401 用单次防重。
- **预估**：0.25 天。

### P1-4 关闭窗口/退出应用时的 Stopped 是异步请求，进程可能先退出

- **文件行**：`app/src/mainwindow.cpp:439-445`（`aboutToQuit` 里调用异步 `reportPlaybackStopped()`）；`app/src/mainwindow.cpp:478-484`（Controller 析构同样）；`app/src/mainwindow.cpp:1724-1753`（`reportPlaybackStopped()` 本身是 QNAM 异步 POST）。
- **触发条件**：播放中直接关闭窗口/退出；特别是 Windows 关机或用户快速结束进程。
- **影响**：Stopped 请求可能未发出/未收到响应；服务端保留播放会话，续播位置不写回；`scheduleHomeRefresh` 也不会执行。
- **修复建议**：退出路径使用有界同步等待（例如将 POST 排队后用局部 QEventLoop/短超时等待 finished），或先持久化“待上报 Stopped”并在下次启动重试；至少保证在 aboutToQuit 阶段等待网络短时间。
- **预估**：0.5 天。

### P1-5 服务端返回的绝对媒体 URL 会沿用全局 Authorization header，可能把 token 发给第三方主机

- **文件行**：`app/src/emby/embyclient.cpp:585-606`（`resolveMediaUrl` 保留任意绝对 URL 的 scheme/host）；`app/src/mainwindow.cpp:1098-1112`（candidate 直接使用 server-provided URL）；`app/src/mpvplayer.cpp:262-282`（header 是全局 `http-header-fields`）。
- **触发条件**：PlaybackInfo 的 `DirectStreamUrl/TranscodingUrl` 是外部 CDN、反代或恶意/被篡改的绝对 URL；或服务器 30x 跳到其他 host。
- **影响**：Authorization 中的 token 会随请求发给非 Emby 源主机；CDN/外部日志可能记录 token。
- **修复建议**：`resolveMediaUrl` 只允许 `http/https`，并校验 host:port 与 profile.baseUrl() 同源；需要 CDN 时显式白名单；mpv 不应全局携带 token 到任意 URL（可考虑按 host 注入 header 的代理层，或只对同源 URL 使用 header）。
- **预估**：0.25 天。

### P1-6 token 以明文 JSON 存在 `QSettings`，无 DPAPI/凭据管理器

- **文件行**：`app/src/ui/tokenstore.cpp:26-31`；`app/src/ui/tokenstore.h:10-14` 自述“临时实现：JSON 明文写入 QSettings”。
- **触发条件**：任何登录成功；本机其他进程/同一用户目录读取 QSettings。
- **影响**：本机持久化的 access token 可被读取后直接调用服务器 API；崩溃转储/备份也会带上。
- **修复建议**：Windows 使用 DPAPI（`CryptProtectData`）或 Credential Manager；非 Windows 用系统 keyring/受限文件权限；补迁移逻辑（旧明文读取后加密并清除）。
- **预估**：1 天。

### P1-7 崩溃转储使用 `MiniDumpWithDataSegs`，可能包含内存中的 token

- **文件行**：`app/src/crashhandler.cpp:177-180`（`MiniDumpWithThreadInfo | MiniDumpWithDataSegs | MiniDumpWithUnloadedModules`）。
- **触发条件**：登录状态下崩溃，用户回传 `crashes/crash-*.dmp`。
- **影响**：token 可能位于进程可写内存段，被写进 minidump；与 NFR-05“token 不出现在崩溃转储明文”冲突。
- **修复建议**：发布版改用 `MiniDumpNormal`（去掉 `MiniDumpWithDataSegs`），或明确崩溃包仅内部使用并做访问控制；崩溃文本中的 `app.log` 片段也先确认已脱敏。
- **预估**：0.5 天。

### P1-8 启动恢复与手动登录/退出无代际保护，迟到回调可覆盖新会话

- **文件行**：`app/src/ui/mainwindow.cpp:1488-1533`（`restoreSession` 的 public info/views 回调）；`app/src/ui/mainwindow.cpp:1566-1603`（`onLoggedIn` 无条件覆盖 profile）；`app/src/ui/mainwindow.cpp:1605-1638`（logout）。
- **触发条件**：启动时自动恢复较慢，用户在此期间登录另一个账号/服务器，或退出登录；旧恢复请求随后成功。
- **影响**：旧 `stored` profile 的 `onLoggedIn(stored, ...)` 会把新会话覆盖回旧账号/旧 token，造成账号串号；旧 `fetchViews` 也缺少 generation 保护（P2-2）。
- **修复建议**：MainWindow 增加 `m_sessionGeneration`，restore/login/logout 各递增；所有恢复回调带 generation 校验；logout 后旧回调直接丢弃；必要时 `abortAll`。
- **预估**：0.25 天。

---

## P2（可随后统一清理）

### P2-1 token 清理键名不完整

- **文件行**：`app/src/emby/embyclient.cpp:62-73` 只清理 `api_key/X-Emby-Token/ApiKey/access_token`。
- **问题**：服务器返回的 URL 若使用 `token`、`accessToken`、`apiKey`、`X-MediaBrowser-Token` 等键，会原样保留并传给 mpv。
- **建议**：统一用大小写不敏感的 token 参数集合，包含上述别名；加测试。

### P2-2 切换 profile 不取消在途请求；`loadRealViews` 无 generation

- **文件行**：`app/src/emby/embyclient.cpp:115-128`（`setProfile` 只赋值）；`app/src/ui/mainwindow.cpp:902-926`（views 回调只检查 `hasSession/mockMode`）。
- **问题**：切换账号/服务器时旧请求继续；旧 views 响应可在新会话中覆盖 `m_sections`；与 P1-8 同源。
- **建议**：EmbyClient 增加请求代际/按 profile 取消；UI 给 loadRealViews 加 generation 或使用 client 代际。

### P2-3 剧集队列固定 Limit=500，且新 series 请求失败不清旧队列

- **文件行**：`app/src/mainwindow.cpp:824-831`（`query.limit = 500`）；`app/src/mainwindow.cpp:845-857`（成功后才清空/重建）。
- **问题**：>500 集的长剧会被截断，目标集若在 500 后会被当成第 1 集；切换剧集请求失败时旧队列/按钮状态残留。
- **建议**：分页拉全或至少按目标集定位；请求开始时清空/禁用旧队列；增加长剧测试。

### P2-4 详情页 Series 的整剧 episode 查询没有 Limit

- **文件行**：`app/src/ui/librarydata.cpp:902-919`。
- **问题**：`fetchDetail` 对 Series 递归查全部 Episode，不设 Limit；大剧集/服务器默认返回全部时内存与延迟不可控。
- **建议**：按季/分页拉取，或设置合理上限并显示“加载更多”。

### P2-5 `preferredMediaSource` 只选第一个 DirectPlay 源，候选 URL 也只围绕该源

- **文件行**：`app/src/emby/embytypes.cpp:517-524`；`app/src/mainwindow.cpp:1080-1112`。
- **问题**：第一个 source 声明 DirectPlay 但 URL 失效/不可达时，不会尝试同一条目的其他 MediaSource；只能在同一 source 的候选 URL 间重试。
- **建议**：返回所有可用 source 的候选，按 DirectPlay→DirectStream→Transcode 排序；失败后切换 source。

### P2-6 DeviceProfile 字幕/直连容器覆盖偏窄

- **文件行**：`app/src/emby/deviceprofile.h:39-57`、`app/src/emby/deviceprofile.cpp:68-83`。
- **问题**：只声明 ass/srt 字幕，PGS/VOBSUB/SUP 未声明，可能触发服务端烧录/转码；直连容器缺 mov/avi/wmv/m2ts 等，Codec 缺 mpeg2video/vc1 等，宽直连率可能下降。
- **建议**：按 libmpv 实际能力补齐；至少补 PGS/SUP 的 Embed 能力和常见容器。

### P2-7 元数据请求无重试/退避

- **文件行**：`app/src/emby/embyclient.cpp:337-378`（sendGet/sendPost）；错误仅回调给 UI。
- **问题**：瞬时网络错误/超时后用户只能手动重试；首页多请求时任一失败会造成局部空态。
- **建议**：对幂等 GET（Views/Items/详情/Resume）做 1–2 次指数退避重试；播放/进度 POST 不做盲目重试或仅幂等处理。

### P2-8 `mpv.log` 无轮转/大小上限

- **文件行**：`app/src/mpvplayer.cpp:284-287`（`log-file`）；`app/src/main.cpp:54-65` 只处理 `app.log` 轮转。
- **问题**：长时间播放/频繁重初始化会让 `mpv.log` 持续增长；导出诊断包变大，且 token 泄露窗口更长。
- **建议**：启动时/大小阈值轮转 `mpv.log`；与 P0-2 的“清理旧日志”一起做。

### P2-9 首次探测 Jellyfin 时仍以默认 Emby 类型发送 legacy 头

- **文件行**：`app/src/emby/embyclient.cpp:238-242`（`serverType == Emby` 才发 legacy，但新连接默认 Emby）；`app/src/ui/logindialog.cpp:160-175`（探测前先 applyServerProfile，serverType 置 Emby）。
- **问题**：对 Jellyfin 的第一次 `/System/Info/Public` 会带 `X-Emby-Authorization/X-Emby-Token`（token 为空）；多数 Jellyfin 会忽略，但严格/未来版本可能拒绝，影响 CON-04 的兼容性。
- **建议**：未确认服务器类型时只发标准 `Authorization`，确认 Emby 后再启用 legacy 头；或先不带认证头探测 public info。

### P2-10 测试覆盖缺口

- **已有**：`app/src/emby/tests/embyclient_test.cpp` 当前 95 个 check 全通过（本次 ctest 验证）；覆盖认证头/body、Views/Items、详情、PlaybackInfo、Resume、直连 URL、三条进度接口、404。
- **缺口**：
  - 没有测试 `mpv.log` 是否含 token（P0-2 无法被现有测试发现）；
  - `app/src/ui/demo_main.cpp:1794-1807` 反而断言图片 URL 带 `api_key`（P0-1 被测试固化）；
  - 没有播放失败/Start 失败后 Stopped 的测试（P1-2）；
  - 没有 401/403 播放中会话过期测试（P1-3）；
  - 没有 Jellyfin legacy 头缺失、绝对 URL 同源校验、timeout/abort、重试/退避的测试；
  - `live_probe` 需真实凭据、默认不入 CI，且不检查 mpv.log/图片 URL。
- **建议**：修 P0/P1 时新增本地 fake server + 本地 HTTP/mpv 日志回归，把 L-05 敏感词扫描纳入 CI。

---

## 已核对通过 / 不需要改的链路

- 元数据 API URL 不带 token：`EmbyClient::makeRequest()` 只放 Authorization/legacy header；`directStreamUrl()/imageUrl()` 本身无 token（`embyclient.cpp:230-248,569-626`）。
- PlaybackInfo 的 DirectPlay/DirectStream/Transcode 分支、source 选择、DeviceProfile 注入、`PlaySessionId` 保留逻辑与测试覆盖基本一致（`embyclient.cpp:492-535,628-668`）。
- mpv header 值中的逗号转义正确；本地 mpv 验证能收到完整 `Authorization` 头（但这也正是 P0-2 的泄露来源）。
- 进度上报的 ItemId/MediaSourceId/PlaySessionId/PositionTicks/IsPaused/PlayMethod/EventName 字段与 Emby 要求一致（`mainwindow.cpp:1676-1753`；`embytypes.cpp:552-569`）。
- `EmbyClient` 的 20s/12s transfer timeout、`abortAll()`、QPointer 回调保护、QNAM 生命周期处理基本正确。
- 图片缓存的 key 本身 token-free，错误串有 `redactSensitive()`；问题只在 P0-1 的实际请求 URL。
- 没有发现其他生产代码把 token 打进 URL 或 qInfo/qWarning 直接打印 token；`app.log` 的 mpv 日志路径有 `redactSensitiveLog()`，但 `mpv.log` 的原始文件没有经过该函数（P0-2）。

## 建议修复顺序

1. **P0-1/P0-2**：先阻断 token 泄露，并同步 `demo_main` 断言、README、L-05 测试；这是发布门禁。
2. **P1-2/P1-3/P1-4**：统一“播放失败/401/退出”三条会话收尾路径；可一起重构 `PlayerController` 的 session 状态机。
3. **P1-1/P1-5**：mpv header 生命周期与 URL 同源校验。
4. **P1-6/P1-7**：token at rest 与崩溃转储。
5. **P1-8/P2-2**：恢复/切换会话代际 + 测试。
6. P2 按测试覆盖/长剧/重试逐项清理。

## 验证基线

- 当前 Emby 单元测试：`ctest --test-dir /tmp/emby-audit-build --output-on-failure` → 1/1 passed；当前 95 个 check。
- P0-2 的本地复现使用系统 libmpv 0.40 + 本地 HTTP 服务，未在产品代码中写入任何 token；报告不包含真实 token。
- 本次只读审查未修改产品代码；`reports/audit-emby.md` 是唯一新增文件。
