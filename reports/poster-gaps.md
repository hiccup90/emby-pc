# 海报缺失排查报告（T20 / T23 / T25）

- 日期：2026-11-05
- 服务器：Emby 4.9.1.90（真实媒体库，凭据仅通过环境变量传入）
- 采样命令：`emby-ui-demo --live-poster-gaps`（每类型最多 100 条）
- 采样字段：`ImageTags`、`BackdropImageTags`、`PrimaryImageAspectRatio`
- 说明：以下为一次性抽样数据；T23 之后的验证全部使用假服务器/offscreen，不再访问真实服务器，
  也不会对真实媒体库循环播放或反复刷新（避免 .strm 上游风控）。

## 采样结果

| 类型 | 总数 | PrimaryImageTag | ThumbImageTag | BackdropImageTag | SeriesPrimaryImageTag | 任一图片 | 完全无图 |
|---|---:|---:|---:|---:|---:|---:|---:|
| Movie | 96 | 95 (99.0%) | 0 (0.0%) | 94 (97.9%) | 0 (0.0%) | 95 (99.0%) | 1 (1.0%) |
| Series | 81 | 81 (100.0%) | 0 (0.0%) | 78 (96.3%) | 0 (0.0%) | 81 (100.0%) | 0 (0.0%) |
| BoxSet | 0 | - | - | - | - | - | - |

完全无图的条目：

- Movie：`纪念中国人民抗日战争暨世界反法西斯战争胜利80周年阅兵式`（id=1056936）

## 结论

1. **源数据基本完整**：Movie 99.0%、Series 100% 有 `PrimaryImageTag`，只有 1 部 Movie 完全没有图片字段。
   因此实机上“有些海报有、有些没有”不是普遍性的源数据缺失。
2. **主因是客户端失败缓存**：旧版 `EmbyThumbnailCache` 在图片请求瞬态失败（超时/500/并发）后把生成占位图写进 LRU，
   且没有失效/重试机制，导致该条目在整个进程生命周期内永远显示占位图。日志中会出现
   `poster load failed ... status=...`，但旧版不会恢复。
3. **次因是回退链不完整**：旧逻辑只处理 `Primary → Series Primary`，没有 Thumb/Backdrop；
   BoxSet/部分条目只有 Thumb 时会显示空海报。
4. 真实源数据缺图（上述 1 部 Movie）无法靠客户端补出真实海报，只能显示占位图；这是预期行为。

## T20 修复

- `EmbyThumbnailCache`：失败不写永久 LRU；记录失败次数 + HTTP 状态 + 重试时间，默认 TTL 60s 后自动重试；
  成功后才 insert。失败日志写入 app.log（itemId、去 token URL、HTTP 状态、错误串），并累计
  requests/success/failures/retries 统计。
- `LibraryData` 海报链：Primary（自身）→ Series/Parent Primary → Thumb（自身）→ Backdrop（自身/父级）；
  BoxSet 缺 Primary 时回退自身 Thumb；Episode/Season 回退 Series/Parent Primary。
- 诊断：`ui::MainWindow::posterStatsSummary()` 暴露海报缓存统计和来源统计（含 no_image 条目数）；
  `app/src/mainwindow.cpp` 的 `diagnosticSummary()` 已把该统计追加到导出文本；
  失败日志会自动进入 app.log，导出诊断包时可一并带走。

## T23 请求风暴修复

实机诊断证据（run 41952931196）：`poster cache: requests=2253 success=9 failed=0 retries=0 last_http=200`，
而 `poster sources: items=131 own_primary=130`。海报 URL 都存在，2253 次网络请求仅 9 次完成，
属于典型的“paint 重复 request + 无 in-flight 去重 + 无并发上限”请求风暴。

根因：旧 `ThumbnailCache::request()` 的 pending 只覆盖 0ms 定时器；进入 `loadAsync()` 后没有
in-flight 标记，delegate 每次重绘都会再次请求同一 URL，QNAM 队列被淹没。

修复：

- `EmbyThumbnailCache` 增加按 `cacheKey(key,size)` 的 in-flight 集合：已发起/已排队直接返回，
  finished（成功、失败、代际丢弃）时移除并启动下一个。
- 并发上限默认 6（`setMaxConcurrent` 可调），超出进入 FIFO 队列；完成一个放行一个。
- 统计口径修正：`requests` 只在真正发起网络请求时 +1；新增 `in_flight`、`queued`、`max`
  快照并写入 `posterStatsSummary`。
- 失败 TTL（默认 60s）重试保留，并统一经过 in-flight/队列去重，避免重试造成重复风暴。

## T25 请求挂起修复

实机证据（run 41957153193 包）：`poster cache: requests=12 success=6 failed=0 retries=0 in_flight=6 queued=32 max=6`。
6 个请求成功后 6 个槽位永久 in-flight、32 个队列卡住，说明图片请求会挂起且没有传输超时；
元数据 QNAM 正常，问题集中在图片 QNAM/HTTP 连接复用。

修复：

- 复用 `EmbyClient::networkManager()` 的 QNAM 发图片请求（元数据正常的那一个）；无则回退自有 QNAM。
- 每请求 `setTransferTimeout(15000)`，超时按失败处理并释放槽位；失败最多重试 2 次（初始 + 2 次）。
- 图片 URL 追加 `api_key` 查询参数（qEmby/Jellyfin 兼容），Authorization 头保留；元数据 URL 仍不含 token，
  日志与诊断中的 URL/错误串对 token 做脱敏。
- `EMBY_PC_IMAGE_HTTP2=0/1` 开关（默认 0=关闭 HTTP/2）用于对比。
- 本地磁盘缓存：`QStandardPaths::CacheLocation/images`，key=itemId/imageType/tag/尺寸，
  先读盘、成功写盘，默认 1GB LRU；`EMBY_PC_IMAGE_DISK_CACHE=0` 可关闭。
- 逐请求 start/finish/timeout 日志（前 20 条 + 失败全部），诊断追加 qnam/http2/timeout/平均耗时/磁盘命中统计。

## 复现/验证（全部假服务器 + offscreen，不访问真实服务器）

- 假服务器自测：首次图片 500、TTL 后第二次 200，确认失败不写 LRU、占位图临时显示、重试成功后缓存恢复。
- 回退链自测：有 Primary、只有 Thumb、BoxSet 只有 Thumb、完全无图、Episode Series Primary 五类均覆盖。
- 请求风暴自测：同一 URL 连续 request 100 次 → 只发起 1 次网络请求；12 张延迟响应图片 →
  6 个在途 + 6 个 FIFO 排队，服务器峰值并发 6，队列完成后每张只请求 1 次。
- 请求挂起自测：假服务器 hold 500ms，图片传输超时 100ms → 失败释放槽位；最多重试 2 次后不再请求；
  本地磁盘缓存写盘后，新 cache 命中磁盘不发起网络请求；图片 URL 带 api_key、Authorization 保留。
- 实机只需正常打开一次媒体库观察 app.log 的 `poster stats`；不要循环播放/反复刷新触发上游风控。
  若仍有缺失，查看 `poster load failed` 的 itemId/HTTP 状态并导出诊断信息附上 poster stats。
