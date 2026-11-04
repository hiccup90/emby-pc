# app/src/ui — Qt Widgets UI 骨架（T4）与登录/会话（T6）

Emby PC 客户端的表现层：**Qt 6 Widgets + 深色 QSS**。T4 完成主窗口/媒体库/详情/播放页骨架（mock 数据），
T6 增加登录对话框、Token 持久化、启动自动恢复与设置页账户信息；媒体库仍为 mock（M2-B 再接真实数据）。

## 目录结构

| 文件 | 职责 |
|---|---|
| `mainwindow.h/.cpp` | 主窗口壳：侧边栏 + 顶栏搜索 + QStackedWidget 页面栈（首页/电影/剧集/设置/详情/播放） |
| `sidebar.h/.cpp` | 左侧导航（首页/电影/剧集/设置）+ 服务器状态卡片 |
| `libraryview.h/.cpp` | 媒体库页：QListView(IconMode) + PosterDelegate 海报网格、搜索过滤、排序、空状态、首页 Hero banner |
| `posterdelegate.h/.cpp` | 海报卡片绘制：圆角海报、评分角标、观看进度条、hover/选中态 |
| `mediamodel.h/.cpp` | `MediaListModel`（QAbstractListModel）+ `MediaFilterProxyModel` 搜索过滤 |
| `detailview.h/.cpp` | 详情页：背景/海报、标题元数据、简介、播放按钮、季/集列表（mock） |
| `playerpage.h/.cpp` | 播放页容器：VideoHost 占位 + 播放/暂停/上下集/字幕/音轨等信号 |
| `thumbnailcache.h/.cpp` | 缩略图缓存接口：同步 get + 异步 request + LRU，默认生成离线占位海报 |
| `postergenerator.h/.cpp` | 确定性抽象海报/背景图生成（mock 资源） |
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

`--selftest` 默认使用本地假 Emby 服务器（QTcpServer），不依赖真实网络；`--live-login` 才访问环境变量指定的真实服务器。

## 与播放器 / 数据层对接

### 1. 视频宿主（mpv --wid）

`PlayerPage` 内是 `VideoHostPlaceholder` 占位。主窗口/播放器模块拿到真实 `VideoHost`（原生子 HWND）后：

```cpp
auto *host = new VideoHost(playerPage);          // app/src/videohost.h，现有骨架
playerPage->attachVideoSurface(host);            // 替换占位，stack 自动切到真实宿主
playerPage->releaseVideoSurface();               // 停止播放/退出时恢复占位
```

### 2. 控制信号（PlayerPage → 播放控制层）

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

### 3. 媒体数据（mock → Emby API）

`LibraryView::setItems(QList<MediaItem>)` 接收 `ui::MediaItem`（见 `uitypes.h`，字段与 Emby DTO 对齐）。
真实接入时由 `app/src/emby` 的映射层填充 `MediaItem` 后调用即可，海报走 `thumbnailcache`：

- 现在：`ThumbnailCache::generate()` 用 `postergenerator` 生成离线占位图。
- 接网络：继承 `ThumbnailCache` 重写 `generate()`（或用 `insert()` 回填 async 下载结果），
  delegate 的同步 `get()` + `thumbnailReady` 重绘机制无需改动。

## 验证结果（本容器，Qt 6.8.2 / CMake 3.31 / GCC 14）

`QT_QPA_PLATFORM=offscreen emby-ui-demo --selftest` 共 **53 项全部 PASS**，包括：

- mock 媒体库 200 部；QListView 模型 200 行；海报网格 gridSize 生效；
- 搜索“星际”过滤为 5 条，清空恢复 200 条；
- 200 项从头滚到底：5 帧平均 **15~18 ms/帧**（offscreen 软件渲染，含整窗 grab，仅作回归基线）；
- 页面栈切换（首页/电影/剧集/详情/播放）索引正确；
- 播放页播放/暂停、上一集、下一集按钮各发出 1 次对应信号；
- `attachVideoSurface()` 可替换占位、`releaseVideoSurface()` 可恢复；
- 登录：假服务器测试连接/登录成功、401 密码错误、连接拒绝中文提示、token 不泄露到状态栏/URL；
- TokenStore 保存/读取 ServerProfile、启动自动恢复、退出登录清除、过期 token 回登录页；
- 设置页服务器/用户/版本显示、`embyClient()` 与 `loginStateChanged(bool)` 信号。

真实 Emby 联调（容器可达、凭据由环境变量传入的服务器）通过：测试连接 → authenticate → fetch Views → 启动自动恢复 token 均成功（凭据仅通过环境变量传入，不写入代码/日志）。

`app/` 根工程完整构建通过（`emby-pc` 可执行文件链接了 `emby` + `ui` 静态库）。

### 预览截图（1280x760 / 960x600）

| 页面 | 截图 |
|---|---|
| 首页（Hero + 海报网格） | [01-home.png](preview/01-home.png) |
| 电影库 | [02-movies.png](preview/02-movies.png) |
| 详情页 | [03-detail.png](preview/03-detail.png) |
| 详情页 · 剧集列表 | [04-detail-episodes.png](preview/04-detail-episodes.png) |
| 播放页（VideoHost 占位 + 控制条） | [05-player.png](preview/05-player.png) |
| 最小尺寸 960x600 | [06-home-960x600.png](preview/06-home-960x600.png) |
| 设置页 | [07-settings.png](preview/07-settings.png) |
| 1080p 首页 | [08-home-1920x1080.png](preview/08-home-1920x1080.png) |
| 最小尺寸详情页 | [09-detail-960x600.png](preview/09-detail-960x600.png) |
| 最小尺寸播放页 | [10-player-960x600.png](preview/10-player-960x600.png) |
| 登录对话框 | [11-login-dialog.png](preview/11-login-dialog.png) |

## 已知限制 / 后续

- 媒体库数据仍为本地 mock（M2-B 接真实 Views/Items）；未接真实播放器（播放页仅状态演示）。
- 海报/背景图为程序生成的抽象图（离线占位），非真实海报。
- 播放器控制条当前为 Qt 控件版（方案 B 的骨架）；最终采用 mpv OSC（方案 A）时，
  `PlayerPage` 的控制条可切换为隐藏/纯状态显示，视频区仍由 `attachVideoSurface` 挂载。
- 主窗口暂用系统标题栏；无边框 DWM 方案（P5）后续在 WindowController 层实现。
- 未实现真实图片异步下载线程池；`ThumbnailCache` 已留好接口与 LRU。
- TokenStore 当前为 QSettings 明文临时实现（Windows DPAPI 待补）。