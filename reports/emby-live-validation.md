# 真实 Emby 服务器联调验证（T10）

> 联调日期：2026-11-04
> 服务器：用户内网 Emby 服务器（Emby 4.9.1.90，地址与服务器名已脱敏）
> 凭据：测试账号与密码只在运行时通过环境变量传给 live_probe，未写入本文件、仓库或其他日志；本文不记录账号、密码、AccessToken。
> 结论：**live_probe 55 项检查全部通过，0 failures**；认证、Views、Items（分页/搜索/排序）、详情、Season/Episode、PlaybackInfo、直连 URL、图片 URL、Resume、错误码均已在真实服务器验证。

## 1. 联调方法

- Harness：`app/src/emby/tests/live_probe.cpp`（Qt6 Core/Network，复用 `emby::EmbyClient`，不依赖 Widgets）。
- 构建：`cmake -S app/src/emby/tests -B /tmp/emby-live-build && cmake --build /tmp/emby-live-build -j`
- 运行（凭据只通过环境变量传入，不落盘）：

```bash
EMBY_URL=... EMBY_USER=... EMBY_PASSWORD=... /tmp/emby-live-build/live_probe
```

- 输出刻意不打印 token/password；只输出服务器名/版本、view/item id、布尔值和字段形状。
- 另用 Python 一次性脚本做了 HTTP 直连流和图片端点的 Range/状态码验证（token 仅保存在进程内存）。

## 2. 验证结果总览

| 项目 | 结果 | 证据 |
|---|---|---|
| `GET /System/Info/Public` | OK | ServerName=`<服务器名>`，Version=`4.9.1.90`；`ProductName` 字段为空，属服务器行为 |
| `POST /Users/AuthenticateByName` | OK | 返回 AccessToken、User.Id；`EmbyClient` 正确保存 token/UserId/UserName |
| `GET /Users/{userId}/Views` | OK | 3 个视图：电视节目/tvshows、Movie/movies、合集/boxsets |
| `GET /Users/{userId}/Items` | OK | 可播总数 `Movie+Episode = 5109`；Movie=96，Series=81，Episode=5013，BoxSet=0 |
| Items 分页 | OK | `StartIndex=0, Limit=10` 与 `StartIndex=10, Limit=10` 均返回 10 条，两页 id 无重叠 |
| Items 排序/搜索 | OK（有服务器怪癖） | `SortBy=DateCreated&SortOrder=Descending` 正常；`SearchTerm` 能返回 Items，但 `TotalRecordCount` 可能为 0（见 §5） |
| `GET /Users/{userId}/Items/{id}` 详情 | OK | 名称、原名、年份、分级、社区评分、简介、时长、Container、Path、Genres、ImageTags、MediaSources、MediaStreams 均解析 |
| Seasons/Episodes | OK | 样例剧集 4 季；第 1 季 18 集；`/Shows/{id}/Episodes` 返回整剧 45 集 |
| `POST /Items/{id}/PlaybackInfo` | OK | PlaySessionId、MediaSources、source id/container/bitrate/runTime/streams 全部解析；宽/窄 DeviceProfile 行为不同（见 §7） |
| 直连流 | OK | `static=true&mediaSourceId=...` 请求 HTTP 206 `video/x-matroska`；服务端 DirectStreamUrl 去掉 api_key 后同样 206 |
| 图片 URL | OK | Primary/Backdrop 均 HTTP 200 `image/jpeg`（约 56 KB / 241 KB） |
| `GET /Users/{userId}/Items/Resume` | OK（空） | 当前库返回 TotalRecordCount=0、Items=[]；UI 需处理空态 |
| 无效 item id | 服务器 500 | Emby 对不存在 id 返回 HTTP 500（不是 404），`ApiError` 正确映射 httpStatus/message |

## 3. 真实库结构与统计（UI 可直接参考）

### 3.1 Views

| 名称 | View Id | collectionType | 说明 |
|---|---|---|---|
| 电视节目 | `17397` | `tvshows` | 剧集库；Series 81 部，Episode 5013 集 |
| Movie | `795183` | `movies` | 电影库；Movie 96 部 |
| 合集 | `1058657` | `boxsets` | 合集视图，当前 BoxSet 条目为 0 |

- 侧边栏建议顺序：电影 → 电视节目 → 合集；`MediaItem.type` 为 `CollectionFolder`，`isFolder=true`，`collectionType` 用于选择图标/路由。
- `ChildCount`/`RecursiveItemCount` 在 Views 响应中不保证存在（本库为 null），不要用来算总数；总数用 Items 的 `TotalRecordCount`（搜索例外见 §5）。

### 3.2 Item 字段形状

列表/详情常见字段（PascalCase，直接对应 `MediaItem`）：

```text
Id, Name, Type, MediaType, CollectionType,
ProductionYear, RunTimeTicks, Container, Path, Size, Bitrate,
OfficialRating, CommunityRating, CriticRating, PremiereDate, DateCreated,
Overview, OriginalTitle, SortName, Genres[],
ImageTags{Primary, Logo}, BackdropImageTags[], ParentBackdropItemId,
ParentBackdropImageTags[], SeriesPrimaryImageTag, ParentLogoItemId, ParentLogoImageTag,
PrimaryImageAspectRatio, IsFolder, IndexNumber, ParentIndexNumber,
SeriesId, SeriesName, SeasonId, SeasonName,
UserData{PlaybackPositionTicks, PlayCount, IsFavorite, Played, UnplayedItemCount},
ProviderIds{Tmdb, Imdb}, People[], Studios[], Taglines[], ProductionLocations[]
```

重要：**Items 列表接口的 `Fields` 是 opt-in**，未请求的字段可能不返回（例如不请求 `ProductionYear` 时它是 null/缺失）。本模块已把常用字段加入 `ItemQuery` 默认 `Fields`；页面仍应按需覆盖：

- 网格/列表：`ProductionYear, PrimaryImageAspectRatio, ImageTags, BackdropImageTags, UserData, Genres`
- 详情页：`RunTimeTicks, Container, Path, OfficialRating, CommunityRating, PremiereDate, DateCreated, Genres, ProviderIds`
- 播放：`MediaSources, MediaStreams`；但真正的播放元数据以 PlaybackInfo 为准（见 §7）

样例 id（用于 UI 本地手工验证，非凭据）：

| 对象 | Id | 名称 |
|---|---|---|
| Movie | `1056815` | 边境奇袭 |
| Series | `1051949` | 爱死亡，机器人 |
| Season 1 | `1053299` | 第 1 季 |
| Episode | `1053321` | 帮手 |

### 3.3 图片 URL 规则

- Primary：`/Items/{itemId}/Images/Primary?tag=<ImageTags.Primary>&maxHeight=300`（或 `maxWidth`）。
- Backdrop：`/Items/{itemId}/Images/Backdrop?tag=<BackdropImageTags[0]>&maxWidth=800`；本库 Backdrop 不要求 tag 也能返回 200。
- **不要**把 token 拼进图片 URL；加载时给 `QNetworkRequest` 加 `Authorization` 头（可用 `EmbyClient::streamHeaderLines()` / `authorizationHeader()` 生成）。
- Season/Episode 通常有自己的 `ImageTags.Primary`；没有时用 `SeriesPrimaryImageTag` + `SeriesId`（季还有 `ParentBackdropItemId`/`ParentBackdropImageTags`）。
- `EmbyClient::imageUrl(itemId, type, tag, maxWidth, maxHeight)` 已封装上述 URL 规则，不附加 token。

## 4. Season / Episode 查询

已验证三种等价方式：

1. 季列表：`GET /Users/{userId}/Items?ParentId={seriesId}&IncludeItemTypes=Season&Recursive=false&Fields=Overview,UserData&SortBy=SortName`
2. 集列表：`GET /Users/{userId}/Items?ParentId={seasonId}&IncludeItemTypes=Episode&Recursive=false&Fields=MediaSources,MediaStreams,Overview,UserData&SortBy=SortName`
3. 整剧集列表：`GET /Shows/{seriesId}/Episodes`（可加 `SeasonId=` 过滤）

样例结果：系列 `1051949` 共 4 季；第 1 季 `1053299` 共 18 集；`/Shows/1051949/Episodes` 全剧 45 集。`Season`/`Episode` 都返回 `IndexNumber`（集号）和 `ParentIndexNumber`（季号，Season 场景下常见为季号）。

## 5. 搜索与分页注意事项

- 分页参数工作正常：`StartIndex`/`Limit` 在 Items 查询中生效；`Limit=10` 的 page1/page2 id 无重复。
- **Emby 4.9.1.90 的 SearchTerm 响应可能出现 `TotalRecordCount=0` 但 `Items` 非空**（本库搜索 `边境`/`的`/`爱` 均为 0 + 非空 Items；搜索 `2025` 返回 2）。UI 不要用 `TotalRecordCount==0` 判断空结果；应以 `Items` 是否为空为准，分页时用“本页是否满页”继续请求。
- `ItemQueryResult::hasMore()` 已针对 `TotalRecordCount==0` 做回退：`pageSize>0 && items.size()>=pageSize` 时认为可能还有下一页。

## 6. 解析校验与本次修复/改进

联调时对每个真实响应都用 `emby::EmbyClient` 解析并断言关键字段，未发现异常类型导致的崩溃或解析错误；同时补了以下适配（均在 emby 模块内，等价于原 T3 模块的小修复）：

| 文件 | 改动 | 原因 |
|---|---|---|
| `app/src/emby/embytypes.h/.cpp` | `MediaItem` 新增 `genres`、`logoImageTag`、`thumbImageTag`，并解析 `Genres`、`ImageTags.Logo/Thumb` | 真实库有 `Genres[]` / `ImageTags.Logo`，详情页可直接用 |
| `app/src/emby/embytypes.h/.cpp` | `MediaUserData` 新增 `unplayedItemCount`，解析 `UnplayedItemCount` | 真实 Series/Season 响应有未看集数 |
| `app/src/emby/embytypes.h` | `ItemQuery` 默认 `Fields` 增加 `ProductionYear, PrimaryImageAspectRatio, DateCreated, OfficialRating, CommunityRating` | 列表页常用字段是 opt-in，避免默认查询拿到 0/空 |
| `app/src/emby/embytypes.cpp` | `ItemQueryResult::hasMore()` 对 `TotalRecordCount==0` 回退为满页判断 | SearchTerm 的服务器怪癖 |
| `app/src/emby/embyclient.cpp` | `fetchItem()` / `fetchResumeItems()` 的 `Fields` 补齐 `RunTimeTicks, Container, Path, Genres, ...` | 详情/继续观看页需要时长、实际容器、路径 |

回归：原有 `app/src/emby/tests/embyclient_test.cpp` 假服务器测试仍为 **94 项断言全部通过**；`g++ -fsyntax-only -Wall -Wextra -Wpedantic` 通过。

## 7. PlaybackInfo 与直连播放验证

样例电影：`1056815`（边境奇袭）。

| 字段 | 值 |
|---|---|
| PlaybackInfo `PlaySessionId` | 非空（每次请求新生成） |
| MediaSource `Id` | `mediasource_1056815` |
| PlaybackInfo `Container` | `mkv`（注意：详情接口 MediaSources 的 Container 是 `strm`，这是 STRM 外链片源） |
| `Size` / `Bitrate` / `RunTimeTicks` | `2233155090` / `3482383` / `51301760000` |
| 视频流 | `h264`，1920×804，8-bit，`VideoRange=SDR`，`ExtendedVideoType=None` |
| 音频流 | `ac3`，6 声道（5.1），spa |
| 字幕流 | `ass`，chi，`IsTextSubtitleStream=true`，默认轨 |
| 宽 DeviceProfile | `SupportsDirectPlay=true`，`DirectStreamUrl` 有值，`TranscodingUrl` 空 |
| 窄 DeviceProfile（关闭 direct play、保留 direct stream/transcode） | `SupportsDirectPlay=false`，`DirectStreamUrl` 有值，`TranscodingUrl` 有值 |

直连验证：

- `EmbyClient::directStreamUrl()` 构造 `/Videos/1056815/stream?static=true&mediaSourceId=mediasource_1056815`；带 `Authorization` 头发起 Range 请求返回 **HTTP 206**，`Content-Type: video/x-matroska`，`Content-Range: bytes 0-1023/2233155090`。
- 服务端 `DirectStreamUrl` 返回 `/videos/1056815/original.mkv?DeviceId=...&MediaSourceId=...&PlaySessionId=...&api_key=...`；`EmbyClient::playbackUrl()` 会保留 `DeviceId/MediaSourceId/PlaySessionId`，剥掉 `api_key`/`X-Emby-Token`；去掉 token 参数后用 `Authorization` 头请求同样 **HTTP 206**。
- 因此 UI/mpv 取流时：**URL 只放路径和 mediaSourceId/PlaySessionId，token 始终走 header**（`streamHeaderLines()` 或 `mpvHeaderFields()`）。

## 8. 给 lead 的遗留风险 / 决策点

1. `GET /System/Info/Public` 在 Emby 4.9.1.90 中 `ProductName` 为空（本库如此）；`ServerInfo::isJellyfin()` 仍能正确返回 false。UI 显示服务器信息时以 `ServerName` + `Version` 为准，不要依赖 ProductName。
2. `SearchTerm` 的 `TotalRecordCount=0` 与 Items 非空问题见 §5；UI 需按 Items 判空和分页，`ItemQueryResult::hasMore()` 已做回退，但 UI 层仍需防御。
3. 当前库 `Resume` 总数为 0；UI 的继续观看区域必须支持空态。
4. 详情接口的 `MediaSources[].Container` 可能是 `strm`，真正的播放容器/码率/流信息要取 PlaybackInfo 的 `MediaSources`；不要把详情里的 source container 直接当播放格式。
5. 本次只验证 Emby 4.9.1.90；Jellyfin 12 / 其他 Emby 版本未验证，尤其是 Authorization header 兼容分支。
6. 容器无 Windows/GPU，未用 libmpv 真正播放；直连流已用 HTTP Range 验证 206 + Matroska，mpv 播放链路需在 Windows 验收包中复测。
7. 服务器对不存在 item id 返回 HTTP 500 而不是 404；UI 错误提示不要把 500 都当成服务器故障（可能是无效/已删除 id）。

## 9. 复跑入口

- 代码：`app/src/emby/tests/live_probe.cpp`，构建配置：`app/src/emby/tests/CMakeLists.txt`
- 报告：本文件；原始 harness 输出在本地 `/tmp/emby-live-probe.out`（不在仓库内，不含 token/password）。
