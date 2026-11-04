# qEmby 拆解报告

> 分析对象：[AlanHJ/qEmby](https://github.com/AlanHJ/qEmby) v0.0.9（Qt 6 Widgets + C++20 + libmpv + QWindowKit，MIT）
> 本地路径：`qEmby/`（含 `libs/qwindowkit` 子模块）
> 配套材料：[`reports/gpu-decode.md`](gpu-decode.md)（GPU 逐行取证）、[`reports/window.md`](window.md)（窗口专项）、[`reports/architecture.md`](architecture.md)（自研选型）

---

## 一、项目速览

| 项 | 内容 |
|---|---|
| UI | Qt 6 Widgets（**CPU 栅格绘制**；作者在 issue #4 里也承认不如 QML/GPU 渲染流畅） |
| 播放 | libmpv（render API）+ `QOpenGLWidget` 内嵌渲染 |
| 窗口 | QWindowKit 无边框窗口，Windows 上 **关闭系统边框**（`CMakeLists.txt:96-107`） |
| 结构 | `src/qEmbyCore`（Emby API / 模型 / 服务）+ `src/qEmbyApp`（视图 / 组件） |
| 播放链路 | Emby API 取流地址 → `MpvController`（mpv 实例）→ `MpvWidget`（在 QOpenGLWidget 里用 mpv render API 画） |

三个问题全部出在下面三层：

1. **mpv 选项层**（`mpvcontroller.cpp`）：决定解码方式、同步方式、是否补帧；
2. **GL 渲染层**（`mpvwidget.cpp` + `windowutils.cpp`）：决定视频像素是否经过 Qt 合成器；
3. **窗口层**（`mainwindow.cpp` / `playbackmanager.cpp` / QWindowKit）：决定最小尺寸与贴边行为。

---

## 二、问题 1：为什么播放时 GPU 占用比其他 Emby 客户端高很多？

### 2.1 一句话结论

**贵的不在"解码"，在"渲染"。** 差异的主因只有两条半：

1. **qEmby 默认把 24fps 的视频补帧到显示器刷新率去渲染**（144Hz 屏 = 每秒画 144 帧，普通客户端只画 24 帧）——这是最大头；
2. **视频像素要穿过 Qt 的 QWidget 合成器**（QOpenGLWidget → 窗口 backing store → DWM），每渲染一帧至少多一次整窗级 blit；qEmby 还用隐藏的 1×1 QOpenGLWidget 把**整个窗口**强制成 GL 合成；
3. （只有开弹幕时）弹幕层再叠一次离屏 FBO + 全屏 blit，并把重绘自驱动到刷新率。

而"硬解 copy-back（`hwdec=auto-copy`）"是**次要成本**——因为 Jellyfin Media Player 默认也是 auto-copy，拿它解释不了"只有 qEmby 高"；它只解释 qEmby 比**浏览器/Emby Theater（真零拷贝）**多出来的那部分。

### 2.2 主因 A：默认开补帧，渲染帧率 = 显示器刷新率（qEmby 独有）

```cpp
// qEmby/src/qEmbyApp/components/mpvcontroller.cpp:145-152
QString vsyncMode = ConfigStore::instance()->get<QString>(
    ConfigKeys::PlayerVideoSync, "display-resample");      // 默认 display-resample
mpv_set_option_string(m_mpv, "video-sync", vsyncMode...);
mpv_set_option_string(m_mpv, "interpolation",
    vsyncMode == "display-resample" ? "yes" : "no");       // 连带强制开补帧
```

* `interpolation=yes` 会沿时间轴卷积补出中间帧（mpv 手册 `--interpolation`），代价是**每个垂直同步都要产出一张新画面**；
* 渲染频率因此从"视频帧率"变成"显示器刷新率"：

| 片源帧率 | 60 Hz | 120 Hz | 144 Hz |
|---|---:|---:|---:|
| 23.976 fps | 2.5×（60 帧/秒） | 5.0× | 6.0×（144 帧/秒） |
| 30 fps | 2.0× | 4.0× | 4.8× |
| 60 fps | 1.0×（阈值内不补） | 2.0× | 2.4× |

* 每一帧都要跑完整的 YUV→RGB 转换 + 缩放 + 抖动 + 合成，不是重复上一帧；
* **qEmby 是反着配的**：mpv 自己的默认是 `video-sync=audio` + `interpolation=no`；JMP 默认也是 `audio` 且全项目从不设 `interpolation`；只有 qEmby 把最费 GPU 的那一档做成了默认（`pageplayer.cpp:76-84` 设置页里 "Display Resample (Default)"）。
* 还有一条隐藏逻辑：默认的 **native 弹幕渲染器**在初始化时会调用 `clearDanmakuMotionStabilityProfile()`（`playerdanmakucontroller.cpp:933 → 875-878`），把 `display-resample + interpolation` **恢复回来**；而被标注 "Not Recommended" 的 ASS 路径反而会切到 `audio`（`:834-835, :955`）。也就是默认路径永远是最贵的那条。

### 2.3 主因 B：`QOpenGLWidget` 嵌在普通 QWidget 窗口里，整窗走 GL 合成

* 视频控件是 `QOpenGLWidget`（`mpvwidget.cpp:23`），`paintGL()` 里调 `mpv_render_context_render()` 画进它的 FBO（`mpvwidget.cpp:359-368`）；
* Qt 官方文档：`QOpenGLWidget` 没有真正的原生表面，paintGL 更新的是 FBO，**"窗口会把该 FBO 纹理与窗口其余内容合成"**。每帧视频因此至少多 1 次整窗 blit + 纹理上传；
* qEmby 再加码：`WindowUtils::prepareOpenGLComposition()`（`windowutils.cpp:13-43`）在**每个顶层窗口创建前**塞一个隐藏的 1×1 QOpenGLWidget 当"锚"（`mainwindow.cpp:97`、`playerwindow.cpp:13`），强制整个窗口走 OpenGL backing store；
* `main.cpp:40` 的 `AA_DontCreateNativeWidgetSiblings` 又禁止所有原生子窗口，UI 全部走 Qt 合成；
* 结果：**窗口多大，每帧就多 blit 多大**。4K 全屏每帧 33 MB，144Hz 单这一项就是约 9.6 GB/s 的读写带宽；即使独显（带宽 300+ GB/s）不卡带宽，288 次/秒的全屏 render pass（144 次 mpv 渲染 + 144 次整窗合成）也是实打实的着色器/ROP 工作量。

> 对比：JMP 虽然也用 libmpv + OpenGL，但走 Qt Quick 的 `QQuickFramebufferObject`（`mpvabstractitem.cpp:25`），纹理只作为 scene graph 的一个纹理节点被采样，**没有 QWidget 的整窗 backing store 合成**。这就是"同样是 GL，JMP 就是低一截"的原因。

### 2.4 主因 C（可选）：弹幕开启时的第三层

* 默认弹幕渲染器是 **native-smooth**（`danmakurendererutils.cpp:15-18`，设置页第一项）；
* 该路径下 `paintGL()` 先渲染视频到离屏 RGBA8 FBO（`mpvwidget.cpp:513-519, 529-540`），再用 `QOpenGLTextureBlitter` 把整张纹理 blit 回默认 FBO（`:560-577`），最后 QPainter 画弹幕 sprite（`:585-586`）；
* `nativedanmakuoverlay.cpp:27-29, 739-761` 还会在 `frameSwapped` 后 `requestNextFrame → update()`，**把重绘自驱动到显示器刷新率**——即使视频只有 24fps；
* 量化：4K/144Hz 下每帧 3 次全屏 surface 操作 ≈ **28.7 GB/s** 带宽，接近主流核显可用带宽上限。1080p/60Hz 也有约 3 GB/s。
* 默认弹幕开关是关的（`playerdanmakucontroller.cpp:63-68`，默认 `false`），所以这条只在看番/开弹幕时生效。

### 2.5 次要项：`hwdec=auto-copy` + Windows 强制 `gpu-api=opengl`

```cpp
// mpvcontroller.cpp:157-174
QString hwdec = ConfigStore::instance()->get<QString>(ConfigKeys::PlayerHwDec, "auto-copy");
...
mpv_set_option_string(m_mpv, "gpu-api", "opengl");   // Windows 上无条件强制 OpenGL
mpv_set_option_string(m_mpv, "hwdec", hwdec...);
```

* `auto-copy` = 解码帧从显存**读回系统内存**再上传成 GL 纹理。4K 10bit（P010）一帧约 25MB，双向各一次；4K24 约 0.6 GB/s、4K60 约 1.5 GB/s，1080p24 约 0.15 GB/s；
* 为什么不能零拷贝？`d3d11va` 零拷贝要求 `--gpu-context=d3d11` 或 `angle`（mpv `DOCS/man/options.rst:1418-1419`）。qEmby 用 libmpv render API + 宿主 OpenGL 上下文，拿不到 D3D11 device，只能用 `*-copy`；设置页的四种硬解选项也**全是 copy 变体**（`pageplayer.cpp:57-62`）；
* 但要纠正一个常见误解：**JMP 默认也是 `auto-copy`**（`resources/settings/settings_description.json` 的 `hardwareDecoding` 默认 "copy"；`PlayerComponent.cpp:1461-1463` 映射为 auto-copy）。所以 copy-back 不是"只有 qEmby 高"的原因；它解释的是 qEmby 相比**浏览器网页版/Emby Theater（D3D11VA 零拷贝 + hardware overlay，基本不经过合成器）**多出来的成本。

### 2.6 量化对比（同机同片）

| 路径 | 解码 | 渲染帧率 | 每帧额外全屏合成 | 相对 GPU 负载 |
|---|---|---|---|---|
| 浏览器网页版 | 零拷贝硬解 + hardware overlay | 视频帧率（24） | 0 | 1×（基准） |
| Emby Theater（Electron） | Chromium/mpv，按视频帧率 | 视频帧率（24） | 0~1 | 1~1.5× |
| Jellyfin Media Player | auto-copy（与 qEmby 同） | 视频帧率（24） | 1 次纹理采样（scene graph 内） | 2~3× |
| **qEmby 默认（无弹幕）** | auto-copy | **屏幕刷新率（60/144）** | **1 次整窗 blit** | **4~10×** |
| **qEmby 默认（开弹幕）** | auto-copy | 屏幕刷新率 | **3 次** | **10~18×** |

具体数字（4K 3840×2160，23.976fps）：
* 普通客户端：24 帧/秒 × 8.3 Mpixel ≈ **200 Mpixel/s** 的着色/合成吞吐；
* qEmby @144Hz：144 帧/秒 × 8.3 Mpixel × 2 次 ≈ **2400 Mpixel/s**；
* 开弹幕再 ×3 ≈ **3600 Mpixel/s**，另加约 28.7 GB/s 的合成带宽。

> 实测一般看到 3~6 倍而不是纸面 6~18 倍，因为固定开销、缓存命中和解码器固定成本会摊薄一部分。

### 2.7 你可以自己验证（不用改代码）

1. 播放时打开播放器菜单里的 **Statistics（统计信息）**：
   * `Refresh Rate` 一行如果是 `144 Hz  23.976 fps`，说明正在按 144Hz 渲染；
   * `Playback` 一行如果写着 `D3D11VA-COPY`（或 DXVA2-COPY / CUDA-COPY），就是 copy-back 硬解；
2. 任务管理器 → 性能 → GPU，引擎类型切到 "3D"/"Copy"：
   * **3D 引擎高** → 补帧/合成在刷帧（主因）；
   * **Copy 引擎高** → copy-back 在搬数据（次因）；
3. 零成本 A/B：**设置 → 播放器 → Video Sync Mode 改成 Audio**（`interpolation` 自动变 `no`），再播同一部片——GPU 通常立刻掉一大半。这是最直接的证据。

### 2.8 修复建议（按性价比排序）

| # | 改动 | 位置 | 预期收益 |
|---|---|---|---|
| 1 | 默认 `video-sync=audio`、`interpolation=no`；补帧改成用户显式开启（文案标注高功耗） | `mpvcontroller.cpp:145-152`、`pageplayer.cpp:76-84`、`playerdanmakucontroller.cpp:71-84` | **最大**，直接砍掉 2.5~6 倍渲染量 |
| 2 | 视频像素不要穿过 Qt 合成器（子 HWND + `--wid`，或 D3D11 render API 自建 swapchain） | `mpvwidget.cpp` 整体 | 大，去掉每帧整窗 blit，并解锁零拷贝 |
| 3 | 删掉 `prepareOpenGLComposition()` 的 1×1 锚、去掉 `AA_DontCreateNativeWidgetSiblings` | `windowutils.cpp:13-43`、`mainwindow.cpp:97`、`playerwindow.cpp:13`、`main.cpp:40` | 中，主窗口不再整窗 GL 合成 |
| 4 | 弹幕层与视频表面解耦（独立透明层），不要每帧离屏 FBO + blit | `mpvwidget.cpp:513-577`、`nativedanmakuoverlay.cpp` | 开弹幕时收益大 |
| 5 | `paintGL` 别阻塞 GUI 线程：传 `MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME=0` 或 `video-timing-offset=0` | `mpvwidget.cpp:359-368` | 改善卡顿，间接减少重绘 |
| 6 | `blend-subtitles=no`（现为 `video`） | `mpvcontroller.cpp:132` | 小~中 |

> 注意第 2 条和现有架构冲突：要零拷贝就得让 mpv 用 D3D11 上下文，而 QOpenGLWidget 提供的是 GL 上下文。所以 qEmby 这套"Qt 内嵌 GL"架构天然只能 copy-back——这也是 issue #15 里作者说下个版本才支持 `vo=gpu-next` 的原因。**要根治必须动渲染架构。**

---

## 三、问题 2：窗口大小有下限，缩不小

直接原因是**两处硬编码**：

```cpp
// 1) 主窗口：进入 Home 模式时
// qEmby/src/qEmbyApp/mainwindow.cpp:1057-1068
setMinimumSize(0, 0);
if (mode == WindowSizeMode::Login) {
    LayoutUtils::fixWindowToContents(this, QSize(m_defaultWidth, m_defaultHeight)); // 登录页固定尺寸
} else if (mode == WindowSizeMode::Home) {
    setMinimumSize(QSize(900, 600).boundedTo(screen()->availableGeometry().size())); // ← 900×600
}
```

```cpp
// 2) 独立播放窗口
// qEmby/src/qEmbyApp/managers/playbackmanager.cpp:851-854
auto* playerWindow = new PlayerWindow(m_core);
playerWindow->setAttribute(Qt::WA_DeleteOnClose, true);
playerWindow->setMinimumSize(960, 540);     // ← 960×540
playerWindow->resize(1280, 720);
```

补充（按影响排序）：
* 登录窗口用 `LayoutUtils::fixWindowToContents`（`layoututils.cpp:13-35`）直接 `setFixedSize`，这是设计如此（登录页 450×320 固定，不需要拉伸）；
* **第二层地板**：把 900×600 改掉以后，下一个卡住你的是标题栏——`QWK--WindowBar QAbstractButton[system-button=true] { min-width: 50px; }`（`dark-style.qss:307-314`）在 Home 页同时显示 **7 个按钮**（back/home/fav/theme/min/max/close），合计 350px；加收藏按钮 10px margin、固定 380px 的搜索框（`mainwindow.cpp:295-297`），标题栏硬地板约 **790~830px**。也就是说光删 900×600 不一定能缩到很小，还得让标题栏在窄窗口下收缩/隐藏按钮；
* 详情页/设置页里大量 `setMinimumWidth` 基本不是元凶——它们都在 `QScrollArea` 里，被滚动区截断了（`detailview.cpp:493`、`settingsview.cpp:133-137` 等）；
* `mainwindow.cpp:1065` 还用了 `.boundedTo(screen()->availableGeometry().size())`，小屏上最小尺寸会等于屏幕尺寸，窗口反而**不能小于屏幕**——1366×768 笔记本上更糟；而且 900px 的最小宽度会让 Windows 分屏时小于 900 的 zone（1366 屏半屏只有 683px）贴不齐，看起来像一条"大缝"。

**改法**：`mainwindow.cpp:1065` 改成 `QSize(640,400)` 或 `0,0`；播放窗口 `0,0`；标题栏按钮 `min-width` 降到 32~36px 并在窄窗口隐藏部分按钮；搜索框改成 `setMinimumWidth(120); setMaximumWidth(380)`。真正要保证的是"内容可滚动"，不是"窗口不能缩小"。

> 另外 `mainwindow.cpp:1068-1074` 已经会打印 `[MainWindow] Window size mode | ... | minimum: ...`，改之前可以先用日志确认当前生效值。

---

## 四、问题 3：窗口贴边有一道缝

qEmby 是 QWindowKit 自绘边框模式：`CMakeLists.txt:96-107` **强制关闭 Windows 系统边框**（`QWINDOWKIT_ENABLE_WINDOWS_SYSTEM_BORDERS=OFF`，注释说是为了避免登录窗展开时露出浅色系统边框）。

QWindowKit 自己的文档就写明了这个模式的代价：关掉系统边框后 **"part of the client edge area will be occupied as the resizing margins"**（`libs/qwindowkit/CMakeLists.txt:48-52`）。具体到代码（`qEmby/libs/qwindowkit/src/core/contexts/win32windowcontext.cpp`）：

1. **主因：1px 的 DWM 外框（全场景，拖边/Aero Snap/分屏都有）**
   ```cpp
   // win32windowcontext.cpp:909-916（winIdChanged）
   if (!isSystemBorderEnabled()) {
       static auto margins = QVariant::fromValue(QMargins(1, 1, 1, 1));
       setWindowAttribute(QStringLiteral("extra-margins"), margins);
   }
   ```
   该属性最终调用 `DwmExtendFrameIntoClientArea(hwnd, {1,1,1,1})`（`:1043-1046`），DWM 会在客户区四边各"吃掉" 1 物理像素去画边框/阴影；窗口同时带着 `WS_THICKFRAME|WS_CAPTION`（`:925-927`），Win11 的 DWM 还会在外圈画它自己的 1px visible frame border（`qwkwindowsextra_p.h:474-490`）。贴边时这就是一条**透出底色的 1px 细缝**；两个窗口接在一起时视觉上约 2px。
   Windows 10 上还会再叠一层：`windows10borderhandler_p.h:37-38, 76-77` 把 `extra-margins` 改成 `QMargins(0, 1, 0, 0)`（顶部再来 1px）。

2. **次因：最大化时按 resize border 宽度内缩客户区（只在最大化时）**
   ```cpp
   // win32windowcontext.cpp:2191-2210（WM_NCCALCSIZE）
   if (max && !full) {
       const quint32 frameSize = getResizeBorderThickness(hWnd); // SM_CXSIZEFRAME + SM_CXPADDEDBORDER
       clientRect->top += frameSize;
       if (!isSystemBorderEnabled()) {
           clientRect->bottom -= frameSize;
           clientRect->left   += frameSize;
           clientRect->right  -= frameSize;
       }
   }
   ```
   `frameSize` 在 100% 缩放是 **8px**（4+4），150% 是 12px。这本身是对"Windows 最大化窗口比工作区大一圈"的正常补偿（上游 1.4.0 同款），**只有实际外扩量和 8px 对不上时（Win11/混合 DPI）才会真留 8px 的缝**。

3. **分数 DPI**：125%/150% 下 Qt 逻辑像素/物理像素取整，再叠 1~2px。

4. **还有一个"大缝"场景**：前面说的 900px 最小宽度会让小于 900 的 Snap 区域（1366 屏半屏只有 683px）贴不齐——这是最小尺寸问题派生出来的。

**怎么判断你遇到的是哪一种**：
* 量缝的宽度：约 1px → 第 1 类（DWM 外框）；约 8px（或 8×缩放比例）→ 第 2 类；只在 125%/150% 缩放时出现 1~2px → 第 3 类；
* 只在最大化时出现 → 第 2 类；拖边/Aero Snap/分屏都有 → 第 1 类；分屏时窗口明显比半个屏幕宽/窄 → 第 4 类（先修最小尺寸）；
* 具体测量：用 WinSpy++ 或 PowerShell 对比 `GetWindowRect`、`GetClientRect`、`DWMWA_EXTENDED_FRAME_BOUNDS`（=9）与显示器 `rcWork`。`reports/window.md` §2.4 有现成的 PowerShell 脚本和判据表。

**改法（按侵入程度）**：
1. 先修最小尺寸（见上一节），顺带解决"分屏贴不齐"的大缝；
2. **去 1px 外框（Win11）**：在 `WindowUtils` 拿到 HWND 后设 `DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR=34, &DWMWA_COLOR_NONE=0xFFFFFFFE, 4)`；不要圆角再加 `DWMWA_WINDOW_CORNER_PREFERENCE = DWMWCP_DONOTROUND`；
3. **最大化 8px 风险**：把 `win32windowcontext.cpp:2205-2209` 的固定 `frameSize` 内缩改成"用 `MonitorFromWindow(hwnd).rcWork` 钳制客户区"——这样无论 Windows 实际外扩多少，客户区都精确等于工作区；或者直接打开 `QWINDOWKIT_ENABLE_WINDOWS_SYSTEM_BORDERS=ON`（代价是 Win10 顶边黑线 workaround 可能露出来，且 fork 里 `setInternalWindowFrameMargins` 被改成直接 `return`（`win32windowcontext.cpp:99-102`），开系统边框前要修掉这个 early return）；
4. **自研客户端**：不要用 QWindowKit 这类自绘边框库——`WM_NCCALCSIZE` 把客户区钳到工作区、`WM_GETMINMAXINFO` 设 `ptMaxPosition/ptMaxSize=rcWork`（QWindowKit 1.4.1 **完全没有处理 WM_GETMINMAXINFO**）、`DWMWA_EXTENDED_FRAME_BOUNDS` 取真实边界（详见 [`reports/architecture.md`](architecture.md) 第 4 节）。

---

## 五、如果自己写一个 Emby PC 客户端

一句话：**Qt 6 + libmpv，但视频表面必须是独立的原生 D3D11 表面，不能经过 Qt 合成器。**

推荐架构（细节见 [`reports/architecture.md`](architecture.md)）：

```
UI（Qt Widgets/QML）
  └─ PlaybackController（状态机/进度上报）
       └─ IVideoSurface ── Windows: 子 HWND + mpv --wid + vo=gpu-next/gpu-context=d3d11/hwdec=d3d11va
                          进阶:  MPV_RENDER_API_TYPE_D3D11 自建 swapchain
                          macOS/Linux: OpenGL render API（QOpenGLWidget/QRhiWidget）
```

必须记住的几条：
1. **硬解永远不要用 `*-copy`**：Windows 用 `hwdec=d3d11va`（配 `gpu-context=d3d11`），其他平台 `auto-safe`；
2. **`video-sync=audio`、`interpolation=no` 写死**；补帧是可选的高功耗功能，不能做默认；
3. **视频像素不要穿过 Qt 合成器**：`QOpenGLWidget` 方案在 4K/高刷下必然多一次全屏 blit；要么子 DXGI swapchain，要么独立窗口/独立 QWindow；
4. **窗口最小尺寸**：顶层窗口 `setMinimumSize(0,0)`，内容用滚动区；不要硬编码 960×540 / 900×600；
5. **无边框窗口**用原生 DWM：`WM_NCCALCSIZE` + `WM_GETMINMAXINFO` + `DWMWA_EXTENDED_FRAME_BOUNDS` + `startSystemResize`，别引第三方自绘边框库；
6. **Emby API**：`/Users/AuthenticateByName` → `/Items` → `/Items/{id}/PlaybackInfo`（带 DeviceProfile）→ 直连 `/Videos/{id}/stream?static=true` → `/Sessions/Playing*` 上报进度；token 走请求头，别拼 URL。

---

## 六、证据索引

| 结论 | 文件:行 |
|---|---|
| 默认 display-resample + interpolation | `qEmby/src/qEmbyApp/components/mpvcontroller.cpp:145-152` |
| 设置页同步默认 display-resample | `src/qEmbyApp/views/settings/pageplayer.cpp:76-84` |
| 弹幕 controller 也以 display-resample 为默认 | `src/qEmbyApp/components/playerdanmakucontroller.cpp:71-84` |
| 默认 auto-copy / 强制 gpu-api=opengl | `mpvcontroller.cpp:157-174` |
| 设置页只有 copy 变体 | `pageplayer.cpp:57-62` |
| QOpenGLWidget 渲染路径 | `src/qEmbyApp/components/mpvwidget.cpp:23, 359-368` |
| 隐藏 1×1 QOpenGLWidget 强制整窗 GL 合成 | `src/qEmbyApp/utils/windowutils.cpp:13-43`、`mainwindow.cpp:97`、`playerwindow.cpp:13` |
| 弹幕离屏 FBO 二次合成 | `mpvwidget.cpp:343-353, 513-577` |
| 弹幕 frameSwapped 自驱动刷新率 | `nativedanmakuoverlay.cpp:27-29, 739-761` |
| 全局 GL 3.3 + 禁止原生子窗口 | `src/qEmbyApp/main.cpp:31-40` |
| 主窗口 900×600 | `src/qEmbyApp/mainwindow.cpp:1065` |
| 播放窗口 960×540 | `src/qEmbyApp/managers/playbackmanager.cpp:853` |
| 标题栏第二层 790~830px 地板 | `src/qEmbyApp/resources/qss/dark-style.qss:307-314, 336-338`；`mainwindow.cpp:295-297` |
| QWindowKit 自述关闭系统边框的代价 | `libs/qwindowkit/CMakeLists.txt:48-52` |
| 系统边框关闭 | `qEmby/CMakeLists.txt:96-107` |
| extra-margins 1px | `libs/qwindowkit/src/core/contexts/win32windowcontext.cpp:909-916, 1043-1047` |
| 最大化 frameSize 内缩 | `win32windowcontext.cpp:2196-2210` |
| Win10 顶部 1px | `libs/qwindowkit/src/core/shared/windows10borderhandler_p.h:37-38, 76-77` |
| JMP 默认 audio、无补帧 | `jellyfin-media-player/resources/settings/settings_description.json`（sync_mode）、`src/player/PlayerComponent.cpp:1449` |
| JMP 默认也是 auto-copy | `settings_description.json`（hardwareDecoding 默认 "copy"）、`PlayerComponent.cpp:1461-1463` |
| mpv d3d11va 需要 d3d11 context | mpv `DOCS/man/options.rst:1418-1419` |
