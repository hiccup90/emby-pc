# app/src/emby — Emby/Jellyfin API 客户端模块

Qt6 Network 异步 Emby API 客户端，供 UI/播放控制器调用。不依赖 Qt Widgets，可在 Linux 容器里单独编译与跑测试。

## 文件

| 文件 | 内容 |
|---|---|
| `embytypes.h/.cpp` | 数据模型：`ServerProfile`、`EmbyUser`、`MediaItem`、`MediaSourceInfo`、`MediaStream`、`PlaybackInfoResult`、`ItemQuery`、`ItemQueryResult`、`ServerInfo`、`AuthResult`、`PlaybackProgressInfo`，均带 `fromJson` / `toJson` |
| `deviceprofile.h/.cpp` | `DeviceProfileOptions` + `DeviceProfile::toJson()`；默认宽直连（mkv/mp4/ts、h264/hevc/av1/vp9、aac/ac3/eac3/dts/truehd/flac/opus、ass/srt），列表和 profile 分组都有开关 |
| `embyclient.h/.cpp` | `EmbyClient`：认证、Views/Items/详情/PlaybackInfo/Resume、直连 URL、进度上报、请求取消 |
| `tests/embyclient_test.cpp` | 基于 `QTcpServer` 的假服务器自测（默认不编译） |

## 接线

根 `app/CMakeLists.txt` 检测到本目录后 `add_subdirectory(src/emby)`，然后：

```cmake
target_link_libraries(emby-pc PRIVATE emby)   # emby 已 PUBLIC 链接 Qt6::Network/Qt6::Core
```

也可以使用本模块导出的 `EMBY_SOURCES` 变量把源码加进已有 target。

## 用法

```cpp
#include "embyclient.h"

using namespace emby;

auto *client = new EmbyClient(this);
client->setServer(QStringLiteral("http://192.168.1.10:8096"));

// 1) 登录：成功后 token / userId / userName 已写入 client->profile()
client->authenticate(username, password,
    [client](const AuthResult &auth, const ApiError &error) {
        if (error.isError()) {
            qWarning() << error.toString();
            return;
        }
        qInfo() << "logged in as" << auth.user.name;
    });

// 2) 浏览
ItemQuery query;
query.parentId = viewId;
query.recursive = true;
query.includeItemTypes = {QStringLiteral("Movie"), QStringLiteral("Episode")};
query.sortBy = {QStringLiteral("SortName")};
query.limit = 60;
client->fetchItems(query, [](const ItemQueryResult &page, const ApiError &error) {
    if (error.isError())
        return;
    for (const MediaItem &item : page.items) { /* ... */ }
});

// 3) 播放信息（每次都要带 DeviceProfile，避免服务端按默认策略转码）
client->fetchPlaybackInfo(itemId, [client, itemId](const PlaybackInfoResult &info,
                                                    const ApiError &error) {
    if (error.isError())
        return;
    const MediaSourceInfo *source = info.preferredMediaSource();
    if (!source)
        return;
    const QUrl url = client->playbackUrl(itemId, *source);  // 直连时无 token
    // libmpv：初始化前用 mpv_set_option_string(mpv, "http-header-fields",
    // client->mpvHeaderFields())；不要使用命令行展开的 -append。
});

// 4) 进度上报（节流由调用方负责，例如每 10s + 暂停/拖动时）
PlaybackProgressInfo progress;
progress.itemId = itemId;
progress.mediaSourceId = sourceId;
progress.playSessionId = info.playSessionId;
progress.positionTicks = mpvPositionTicks;
progress.isPaused = false;
client->reportPlaybackProgress(progress);
```

### 认证/Token 约定

- 所有 API 请求带 `Authorization: MediaBrowser Client="...", Device="...", DeviceId="...", Version="...", Token="..."`。
- 对 Emby 服务器默认额外发送 `X-Emby-Authorization` / `X-Emby-Token`（`ServerProfile::legacyEmbyHeaders` 可关）；Jellyfin 12 已移除旧头，`serverType=Jellyfin` 时不会发送。
- 直连播放 URL 只含 `static=true&mediaSourceId=...`；token 通过 mpv 的 `http-header-fields` 传入，绝不拼进 URL。

## 构建与自测（不需要 Windows/GPU）

```bash
cmake -S app/src/emby -B build/emby -DEMBY_BUILD_TESTS=ON
cmake --build build/emby -j
ctest --test-dir build/emby --output-on-failure
```

`tests/embyclient_test.cpp` 会启动一个本地 `QTcpServer` 假服务器，覆盖认证头/请求体、Views/Items 查询参数、详情、PlaybackInfo（DeviceProfile 字段）、Resume、直连 URL（无 token）、进度上报三条接口和错误处理。
