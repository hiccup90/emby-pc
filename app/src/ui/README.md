# app/src/ui — Qt Widgets UI（T4）+ 登录/会话（T6）+ 真实媒体库（T7 / M2-B）+ 播放器全窗口（T16）

Emby PC 客户端的表现层：**Qt 6 Widgets + 深色 QSS**。T4 完成主窗口/媒体库/详情/播放页骨架，
T6 增加登录对话框、Token 持久化、启动自动恢复，T7（M2-B）把 mock 数据替换为真实 Emby API：
/Views 动态侧边栏、/Items 分页/排序/搜索、首页 Resume + 最近添加、真实海报异步加载、真实详情（季/集）。
设置页保留“演示数据（Mock）”开关，未登录/离线时仍可回归 UI。

## 目录结构

| 文件 | 职责 |
|---|---|
| `mainwindow.h/.cpp` | 主窗口壳：动态侧边栏 + 顶栏搜索 + 页面栈（首页/媒体库/设置/详情/播放），负责真实/mock 数据源切换 |
| `sidebar.h/.cpp` | 左侧导航：首页 + 来自 /Views 的动态媒体库列表 + 设置 + 服务器状态卡片 |
| `homeview.h/.cpp` | 首页：继续观看（Resume 横向卡片）+ 最近添加（复用 LibraryView 海报网格） |
| `libraryview.h/.cpp` | 媒体库页：QListView(IconMode) + PosterDelegate 海报网格、服务端分页/排序/搜索、加载/空/错误状态 |
| `librarydata.h/.cpp` | UI 数据层：Views / Items 分页 / Home(Resume+最近添加) / 详情(季集) 的 Emby 请求编排与 DTO 映射 |
| `posterdelegate.h/.cpp` | 海报卡片绘制：圆角海报、评分角标、观看进度条、hover/选中态 |
| `mediamodel.h/.cpp` | `MediaListModel`（QAbstractListModel）+ `MediaFilterProxyModel` 搜索过滤 |
| `detailview.h/.cpp` | 详情页：背景/海报、标题元数据、简介、播放按钮、真实季/集列表 |
| `playerpage.h/.cpp` | 播放页容器：视频区铺满整页，顶层半透明浮层控制条（自动隐藏/鼠标轮询） |
| `thumbnailcache.h/.cpp` | 缩略图缓存接口：同步 get + 异步 request + LRU，默认生成离线占位海报 |
| `embythumbnailcache.h/.cpp` | 真实 Emby 图片缓存：Authorization 异步下载；in-flight 去重 + 并发上限 6/FIFO；失败 TTL 重试 |
| `postergenerator.h/.cpp` | 确定性抽象海报/背景图生成（离线占位资源） |
| `icons.h/.cpp` | 24x24 线性矢量图标集，不依赖系统主题 |
| `mocklibrary.h/.cpp` | 200 部 mock 媒体库（电影/剧集、季/集、观看进度，确定性） |
| `logindialog.h/.cpp` | 登录/连接对话框：测试连接、登录、HTTPS 自签开关、中文错误提示 |
| `tokenstore.h/.cpp` | TokenStore 抽象 + QSettingsTokenStore 临时实现（预留 Windows DPAPI） |
| `authutils.h/.cpp` | 服务器地址规范化 + ApiError → 中文提示 |
| `settingspage.h/.cpp` | 设置页：账户（服务器/用户/版本/Token 状态、登录/退出）+ 播放/界面/关于 |
| `theme.h/.cpp` + `style/dark.qss` | 深色主题（Fusion + QPalette + QSS，QSS 编译进 Qt 资源） |
| `demo_main.cpp` | 独立 demo / headless 自测 / 截图入口（仅 standalone 构建） |
| `CMakeLists.txt` | 自有构建脚本：`add_library(ui STATIC ...)`；不改 `app/CMakeLists.txt` |

## 构建

### 作为 app 的子模块（正式方式）

`app/CMakeLists.txt` 已约定自动检测并接入：

```cmake
add_subdirectory(src/ui)
target_link_libraries(emby-pc PRIVATE ui)
```

公开头文件包含路径为 `app/src`，所以调用方写：

```cpp
#include "ui/mainwindow.h"
#include "ui/theme.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    ui::Theme::apply(app);           // MainWindow 构造时也会自动调用一次
    ui::MainWindow window;           // 默认 requireAuthentication=true：无会话会弹登录框
    window.show();
    return app.exec();
}

// 只调试 UI、不登录（mock 数据）：
// ui::MainWindow window(ui::MainWindow::Options{false});
```

### 单独构建（调试 / 截图 / 自测）

```bash
cmake -S app/src/ui -B /tmp/emby-ui-build2 -DCMAKE_BUILD_TYPE=Debug -DEMBY_UI_BUILD_DEMO=ON
cmake --build /tmp/emby-ui-build2 -j"$(nproc)"

# 无显示器环境（本容器）：offscreen 平台跑自测/截图
QT_QPA_PLATFORM=offscreen /tmp/emby-ui-build2/emby-ui-demo --selftest
QT_QPA_PLATFORM=offscreen /tmp/emby-ui-build2/emby-ui-demo --screenshot app/src/ui/preview

# 真实服务器数据链路（登录 -> Views -> 首页 -> 分页 -> 详情 -> 季/集 -> 真实海报）
QT_QPA_PLATFORM=offscreen EMBY_TEST_URL='http://<server>:8096' \
  EMBY_TEST_USER='<user>' EMBY_TEST_PASSWORD='<pass>' \
  /tmp/emby-ui-build2/emby-ui-demo --live-data

# 海报缺图抽样（Movie/Series/BoxSet 各最多 100 条，输出 Markdown 表）
QT_QPA_PLATFORM=offscreen EMBY_TEST_URL='http://<server>:8096' \
  EMBY_TEST_USER='<user>' EMBY_TEST_PASSWORD='<pass>' \
  /tmp/emby-ui-build2/emby-ui-demo --live-poster-gaps

# 有桌面环境时直接预览（demo 默认跳过登录）
/tmp/emby-ui-build2/emby-ui-demo
```

## 登录 / 会话（T6）

### 流程

1. `ui::LoginDialog`：服务器 URL、用户名、密码、可选“忽略 HTTPS 证书错误”。
   - “测试连接”调用 `emby::EmbyClient::fetchPublicSystemInfo`，显示服务器名与版本；
   - “登录”先校验服务器，再调用 `authenticate`；成功后发出 `loginSucceeded(ServerProfile)`。
2. `ui::MainWindow` 收到登录结果后通过 `ui::TokenStore` 保存 `ServerProfile`
   （URL / UserId / UserName / AccessToken / DeviceId 等，见 `emby::ServerProfile::toJson()`）。
3. 启动时若 `TokenStore` 中有 token：先 `fetchPublicSystemInfo`，再以 `fetchViews` 验证 token；
   验证失败（401/403）会清除 token 并回到登录框。
4. 设置页“账户”显示当前服务器/用户/服务器版本，提供“登录…”与“退出登录”。

### Token 安全

- `TokenStore` 是抽象接口，当前唯一实现 `QSettingsTokenStore` 为**临时明文**存储
  （`QSettings("auth/profile")` 中的 JSON）；文件/日志中不打印 token，不把 token 拼进 URL。
- Windows 后续实现 DPAPI（`CryptProtectData` / `CryptUnprotectData`）时只需替换 TokenStore 实现，
  UI 与 MainWindow 不用改。
- 所有 API 请求由 `EmbyClient` 在 Authorization 头里携带 token（见 emby 模块 README），
  `imageUrl` / `directStreamUrl` 均不带 token。

### 给播放链路（T8）的接口

```cpp
ui::MainWindow window;
emby::EmbyClient *client = window.embyClient();   // 登录成功后 token/device 已就绪
bool loggedIn = window.hasSession();
// 登录成功发 loginStateChanged(true)，退出登录/会话失效发 loginStateChanged(false)
```

### 真实服务器联调（不写死凭据）

```bash
QT_QPA_PLATFORM=offscreen EMBY_TEST_URL='http://<server>:8096' \
  EMBY_TEST_USER='<user>' EMBY_TEST_PASSWORD='<pass>' \
  /tmp/emby-ui-build2/emby-ui-demo --live-login
```

`--selftest` 默认使用本地假 Emby 服务器（QTcpServer），不依赖真实网络；`--live-login` / `--live-data` 才访问环境变量指定的真实服务器。

## 真实媒体库（T7 / M2-B）

### 数据流

1. **Views → 侧边栏**：登录/恢复会话成功后调用 `GET /Users/{userId}/Views`，
   过滤 `CollectionFolder` / `UserView`，按 movies / tvshows / boxsets 排序后生成动态导航。
2. **Items 分页**：`GET /Users/{userId}/Items?ParentId=...&StartIndex=&Limit=60&SortBy=&SearchTerm=`；
   - 滚动条接近底部（<420px）自动请求下一页，`StartIndex` 递增；
   - 排序（最近添加 / 名称 / 评分 / 年份）由服务器 `SortBy/SortOrder` 完成；
   - 搜索走服务器 `SearchTerm`（不在已加载的 60 条里本地过滤）；
   - Emby 4.9 对 `IncludeItemTypes=Series` 的 `TotalRecordCount` 会按“集数”计数，
     数据层在单页不足 60 个剧集时自动补页，避免剧集库首屏只有 5 部。
3. **首页**：`/Users/{userId}/Items/Resume` 横向“继续观看” + `/Items?SortBy=DateCreated` 最近添加 24 项。
4. **海报**：`LibraryData` 用 `EmbyClient::imageUrl()` 生成不含 token 的 URL 作为缓存 key；
   `EmbyThumbnailCache` 用 `QNetworkAccessManager` 异步下载，请求头带
   `Authorization: MediaBrowser ...Token="..."`；失败时回填离线占位图，避免每次重绘重复请求。
5. **详情**：`GET /Users/{userId}/Items/{id}`；剧集再并行拉 `IncludeItemTypes=Season` 与
   `IncludeItemTypes=Episode`，按 `ParentIndexNumber` 分组。列表页已带 Fields，点开详情先用列表数据
   立即渲染，再异步补齐季/集。
6. **mock 开关**：设置页“界面 → 数据源 → 使用演示数据（本地 Mock）”。关闭时若无会话会拉起登录框；
   登录成功后自动切到真实数据。登录/退出会清空图片缓存，避免串号。

### 错误 / 空 / 加载状态

- 首屏：`LibraryView::setInitialLoading(true)` 显示“正在加载 ……”；
- 空结果：区分“媒体库没有内容”和“搜索无匹配”；
- 请求失败：展示 `friendlyApiError()` 中文错误（连接拒绝、超时、证书、401/403、5xx）；
- 分页失败：保留已加载内容，只在状态栏提示“加载更多失败”。

## 与播放器 / 数据层对接

### 1. 视频宿主（mpv --wid）

`PlayerPage` 内是 `VideoHostPlaceholder` 占位。主窗口/播放器模块拿到真实 `VideoHost`（原生子 HWND）后：

```cpp
auto *host = new VideoHost(playerPage);          // app/src/videohost.h，现有骨架
playerPage->attachVideoSurface(host);            // 替换占位，stack 自动切到真实宿主
playerPage->releaseVideoSurface();               // 停止播放/退出时恢复占位
```

### 2. 播放器模式与浮层（T16 / T18）

进入播放时整个主窗口客户区都是视频（黑边由 mpv 按宽高比绘制），侧边栏/顶栏/状态栏隐藏；
顶部返回/标题条与底部控制条是**独立顶层半透明窗口**（Tool/Frameless/Translucent/不抢焦点），
压在 mpv 原生子窗口之上，不占布局高度，并自动隐藏：

```cpp
ui::MainWindow window;
window.setPlayerMode(true);    // 隐藏 chrome，栈切到 PlayerPage 并铺满
window.setPlayerMode(false);   // 恢复进入前的页面 + 侧边栏/顶栏/状态栏
bool active = window.isPlayerMode();
ui::PlayerPage *player = window.playerPage();

player->setOverlayAutoHide(true);      // 默认开启
player->setOverlayHideDelay(2500);     // 静止 2.5s 隐藏
player->setControlsVisible(true);      // 手动显示/隐藏
```

- 顶层 overlay 由 `PlayerPage` 管理：安装 eventFilter 到主窗口，跟随 Move/Resize/Show/Hide/
  WindowStateChange；底部条宽度 = 主窗口客户区宽度、高 96、贴底；顶部条左上角、宽度自适应内容。
- 自动隐藏：`QTimer` 每 150ms 轮询 `QCursor::pos()`（不依赖 mpv 子窗口的 mouse move）：
  光标在主窗口内移动 → 显示并重置计时；静止 2.5s → 隐藏；暂停时保持显示。
- 播放页背景纯黑（`#000`）；`EMBY_PC_PLAYER_OVERLAY=0` 时退回页面内浮层（排障/无合成器环境）。

### 3. 控制信号（PlayerPage → 播放控制层）

| 信号 | 触发 |
|---|---|
| `playPauseRequested()` | 播放/暂停按钮 |
| `previousEpisodeRequested()` / `nextEpisodeRequested()` | 上一集/下一集 |
| `seekRequested(qint64 positionMs)` | 拖动进度条释放 |
| `subtitleTrackSelected(QString trackId)` | 字幕菜单选择 |
| `audioTrackSelected(QString trackId)` | 音轨菜单选择 |
| `volumeChanged(int volume)` | 音量滑杆（0~100） |
| `fullscreenRequested()` | 全屏按钮 |
| `statsRequested()` | 统计按钮 |
| `backRequested()` | 返回按钮 |

回填接口：`setEpisodeInfo(title, subtitle)`、`setPosition(posMs, durationMs)`、`setPlaying(bool)`、
`setSubtitleTracks(QList<TrackInfo>)`、`setAudioTracks(...)`、`setVolume(int)`、`setFullscreen(bool)`。

### 4. 媒体数据（真实 Emby API）

`ui::LibraryData` 负责把 `emby::MediaItem` 映射成 `ui::MediaItem`（见 `uitypes.h`），
`LibraryView::setItems()/appendItems()` 与 `HomeView::setRecentItems()` 只消费 UI DTO。

- 真实图片：`EmbyThumbnailCache` 继承 `ThumbnailCache::loadAsync()`，按 URL key 异步下载；
  delegate 的同步 `get()` + `thumbnailReady` 重绘机制不变。
- **失败重试（T20）**：失败不写永久 LRU；单独记录失败次数/HTTP 状态/retryAt，默认 60s TTL 后自动重试，
  成功后才 insert 并清理失败记录。失败日志（itemId、去 token URL、HTTP 状态、错误串）写入 app.log；
  `posterStatsSummary()` 暴露 requests/success/failures/retries 与来源统计。
- **请求风暴修复（T23）**：按 cacheKey(key,size) 做 in-flight 去重；并发上限默认 6（可调），
  超出进入 FIFO 队列；requests 只统计真正发起的网络请求，统计追加 in_flight/queued/max。
- **海报回退链（T20）**：Primary（自身）→ Series/Parent Primary → Thumb（自身）→ Backdrop（自身/父级）；
  BoxSet 缺 Primary 时用自身 Thumb；Episode/Season 缺自身图时用 Series/Parent Primary。
- mock 图片：非 http key 仍走 `postergenerator` 离线占位图。
- 播放链路（T8）可直接使用 `LibraryData` 已请求的 Fields（Duration/MediaSources 等按需补充），
  或调用 `MainWindow::embyClient()` 自行请求 `/Items/{id}/PlaybackInfo`。

真实海报缺失的采样与结论见 [reports/poster-gaps.md](../../../reports/poster-gaps.md)
（Movie 95/96 有 Primary、Series 81/81；仅 1 部 Movie 源数据完全无图，其余历史缺图主因是旧版失败后永久缓存占位图）。

## 验证结果（本容器，Qt 6.8.2 / CMake 3.31 / GCC 14）

`QT_QPA_PLATFORM=offscreen emby-ui-demo --selftest` 共 **110 项全部 PASS**，包括：

- mock：媒体库 200 部、首页 Resume 卡片、电影/剧集复用同一 LibraryView、页面栈索引；
- QListView 模型 200 行、海报 gridSize 生效、搜索“星际”5 条、滚动平均 15~18 ms/帧；
- 播放页信号、`attachVideoSurface()/releaseVideoSurface()`；
- T16/T18 播放器模式：侧边栏/顶栏/状态栏隐藏、视频区铺满客户区；
  - 顶层 overlay 创建（Tool/Frameless/Translucent/不抢焦点）、跟随窗口 resize/move、贴边几何；
  - 自动隐藏（注入光标位置，静止 180ms 测试值后隐藏、移动恢复、暂停保持）；
  - 退出播放器模式恢复 chrome 并隐藏 overlay；`EMBY_PC_PLAYER_OVERLAY=0` 页面内兜底；
- 假 Emby 服务器（QTcpServer）上的真实链路：
  - 登录/401/连接拒绝/TokenStore 持久化/启动自动恢复/过期 token 回登录页；
  - /Views 生成 2 个动态媒体库按钮；
  - 首页最近添加来自 /Items；
  - 服务器端 `SearchTerm` 搜索 + 空状态；
  - 真实详情季/集渲染 EpisodeRow；
  - 海报图片请求带 Authorization 且 token 不在 URL；
  - T20：首次图片 500 不写永久 LRU、TTL 后第二次 200 恢复；Primary/Thumb/BoxSet Thumb/无图/
    Episode Series Primary 五类回退链断言；
  - T23：同一 URL request 100 次仅发 1 次网络请求；12 张延迟图片并发上限 6 + FIFO 队列完成；
  - 设置页 mock 开关可来回切换数据源。
- `embyClient()`、`loginStateChanged(bool)`、`MainWindow::visibleItems()` 等 T8 接口可用。

真实 Emby 联调（容器可达、凭据由环境变量传入）`--live-data` 全部 PASS：
登录 → Views（Movie / 电视节目 / 合集）→ 首页最近添加 24 项 → **真实海报下载成功并写入 LRU**
→ Movie 库首屏 60/96 → 滚动分页 60→96 → 真实详情 → 剧集详情 1 季 28 集。
凭据仅通过环境变量传入，不写入代码/日志。

`--live-poster-gaps` 抽样结果：Movie 95/96 有 Primary（1 部源数据完全无图）、Series 81/81，
详细报告见 [reports/poster-gaps.md](../../../reports/poster-gaps.md)。

`app/` 根工程完整构建通过（`emby-pc` 可执行文件链接了 `emby` + `ui` 静态库）。

### CI / 跨平台注意（Windows MSVC）

- 所有 `qBound/std::min/std::max` 的实参类型一致；`StackPage` 与 `int` 混用处已显式
  `static_cast<int>`，避免 MSVC 模板推导歧义（GCC 较宽松）。
- 枚举值保持唯一（每个 `MainWindow::Page` 对应独立值，别名仅通过等价逻辑处理）。
- 未使用参数用 `Q_UNUSED`；`QList` 与 `QVector` 在 Qt 6 已统一，代码未使用差异 API。

### 预览截图（1280x760 / 960x600）

| 页面 | 截图 |
|---|---|
| 首页（继续观看 + 最近添加） | [01-home.png](preview/01-home.png) |
| 媒体库（真实/演示数据共用） | [02-movies.png](preview/02-movies.png) |
| 详情页 | [03-detail.png](preview/03-detail.png) |
| 详情页 · 剧集列表 | [04-detail-episodes.png](preview/04-detail-episodes.png) |
| 播放页（VideoHost 占位 + 控制条） | [05-player.png](preview/05-player.png) |
| 最小尺寸 960x600 | [06-home-960x600.png](preview/06-home-960x600.png) |
| 设置页 | [07-settings.png](preview/07-settings.png) |
| 1080p 首页 | [08-home-1920x1080.png](preview/08-home-1920x1080.png) |
| 最小尺寸详情页 | [09-detail-960x600.png](preview/09-detail-960x600.png) |
| 最小尺寸播放页 | [10-player-960x600.png](preview/10-player-960x600.png) |
| 登录对话框 | [11-login-dialog.png](preview/11-login-dialog.png) |
| **播放器模式 1280x760（视频铺满 + 浮层）** | [12-player-mode-1280x760.png](preview/12-player-mode-1280x760.png) |
| **播放器模式 960x600（视频铺满 + 浮层）** | [13-player-mode-960x600.png](preview/13-player-mode-960x600.png) |
| **播放器模式 · 浮层自动隐藏后（纯视频）** | [14-player-mode-autohide.png](preview/14-player-mode-autohide.png) |

## 已知限制 / 后续

- 未接真实播放器（播放页仅状态演示）；播放链路在 T8/T17 接 mpv/PlaybackInfo。
- 顶层 overlay 在 Windows 上需要实测点击穿透/焦点行为（已用 Tool + WindowDoesNotAcceptFocus +
  ShowWithoutActivating 避免抢焦点；若某些系统仍激活窗口，T17 可加 WS_EX_NOACTIVATE）。
- 自动隐藏基于全局光标轮询，多显示器/高 DPI 下依赖 QCursor 全局坐标；已用主窗口 mapToGlobal 对齐。
- 真实海报已按 Authorization 异步下载；失败时显示临时占位并 TTL 重试，完全无源图时才长期占位。
- 源数据完全无图的条目（本次采样 1 部 Movie）无法显示真实海报，只能占位。
- 播放器控制条当前为 Qt 控件版（方案 B 的骨架）；最终采用 mpv OSC（方案 A）时，
  `PlayerPage` 的控制条可切换为隐藏/纯状态显示，视频区仍由 `attachVideoSurface` 挂载。
- 主窗口暂用系统标题栏；无边框 DWM 方案（P5）后续在 WindowController 层实现。
- 图片请求未做并发上限/取消（LRU + 重复 key 合并已生效），后续可加队列。
- TokenStore 当前为 QSettings 明文临时实现（Windows DPAPI 待补）。
- 真实首页最近添加固定 24 项；“查看全部”尚未做独立页面（媒体库页可浏览全部）。