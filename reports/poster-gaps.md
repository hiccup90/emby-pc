# 海报缺失排查报告（T20）

- 日期：2026-11-05
- 服务器：Emby 4.9.1.90（真实媒体库，凭据仅通过环境变量传入）
- 采样命令：`emby-ui-demo --live-poster-gaps`（每类型最多 100 条）
- 采样字段：`ImageTags`、`BackdropImageTags`、`PrimaryImageAspectRatio`

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

## 复现/验证

- 假服务器自测：首次图片 500、TTL 后第二次 200，确认失败不写 LRU、占位图临时显示、重试成功后缓存恢复。
- 回退链自测：有 Primary、只有 Thumb、BoxSet 只有 Thumb、完全无图、Episode Series Primary 五类均覆盖。
- 实机复测建议：清空图片缓存/重启应用后滚动一次媒体库；若仍有缺失，查看 app.log 中
  `poster load failed` 的 itemId/HTTP 状态，并导出诊断信息附上 poster stats。
