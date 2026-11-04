# qEmby 播放时 GPU 占用偏高的根因分析（代码取证版）

> 分析对象：`/root/github/emby-pc/qEmby`（只读，HEAD `f551964`），Windows + Qt6 Widgets + libmpv render API 路径。
> 所有 qEmby 结论均带 `file:line`；外部依据给官方文档 URL。单位约定：1 MB = 10^6 B，1 GB = 10^9 B。
> 报告日期：2026-11-04

## 0. 结论摘要（先看 5 条）

1. **第一主因是默认的 `video-sync=display-resample` + 连带强制 `interpolation=yes`**（`mpvcontroller.cpp:145-149`，三处默认值一致：`pageplayer.cpp:84`、`playerdanmakucontroller.cpp:75`）。display-* 模式要求按显示器刷新率出帧，interpolation 再把每个刷新周期补成一张新帧 → 24fps 片源在 60/120/144Hz 屏上变成 **60/120/144 次渲染/秒（2.5× / 5× / 6×）**。mpv 自身默认是 `video-sync=audio` + `interpolation=no`（`/tmp/options.rst:6800-6805`、`:5295-5306`），所以这是 qEmby 独有的默认值选择，不是 mpv 行为。
2. **第二主因是渲染载体：QOpenGLWidget 嵌在普通 QWidget 窗口里**（`mpvwidget.cpp:23`）。Qt 文档明确 QOpenGLWidget 先渲染进自己的 FBO，再由顶层窗口把该 FBO 纹理与整个窗口合成（[QOpenGLWidget](https://doc.qt.io/qt-6/qopenglwidget.html)）→ **每个渲染帧至少多 1 次整窗级 blit**。qEmby 还通过 `AA_DontCreateNativeWidgetSiblings`（`main.cpp:40`）和每个顶层窗口预埋隐藏 1×1 QOpenGLWidget（`windowutils.cpp:13-43`，调用点 `mainwindow.cpp:97` / `playerwindow.cpp:13`）把整窗强制成 GL 合成，使这次 blit 无法走原生子窗口/直出路径。
3. **弹幕开启时叠加第三层：默认 native-smooth 渲染器每帧多 2 次全屏 surface 操作**（`danmakurendererutils.cpp:15-18` 默认 native；`mpvwidget.cpp:513-540` 先渲染到离屏 RGBA8 FBO，`:560-577` 再用 `QOpenGLTextureBlitter` blit 到默认 FBO），且 `frameSwapped → requestNextFrame → update()`（`nativedanmakuoverlay.cpp:27-29, 746-761`）把重绘自驱动到显示器刷新率。**4K/144Hz 下仅这三次全屏 surface 操作就约 28.7 GB/s 带宽**，接近主流核显可用带宽的上限。注意：native 路径还会 `clearDanmakuMotionStabilityProfile()`（`playerdanmakucontroller.cpp:933 → 875-878`）**恢复 display-resample + interpolation**；只有被标注"Not Recommended"的 ASS 路径才把 `video-sync` 切到 `audio`（`:834-835, :955`）。默认路径恰好是最贵的那条。
4. **次要但真实的成本：`hwdec=auto-copy` + Windows 强制 `gpu-api=opengl`**（`mpvcontroller.cpp:157-174`）。copy-back 每解码帧做一次 GPU→系统内存回读 + 一次 GL 纹理上传（4K24 约 0.60 GB/s，4K60 约 1.49 GB/s）；`d3d11va` 零拷贝要求 `--gpu-context=d3d11` 或 `angle`（`/tmp/options.rst:1246-1248`），qEmby 的 GL 路径用不了，设置页四种硬解选项全是 `*-copy`（`pageplayer.cpp:57-61`）。**但需纠正一个常见误解：Jellyfin Media Player 默认也是 copy-back**（`resources/settings/settings_description.json:301-304` 默认 `"copy"` → `auto-copy`，`PlayerComponent.cpp:1461-1463`），所以 copy-back 解释不了"只有 qEmby 高"，它是 4K 场景的可优化项，不是差异主因。
5. **已排除的嫌疑**：渲染路径没有任何 `grabFramebuffer/toImage/glReadPixels/QWidget::grab`（grep 0 命中；`grab()` 仅用于主窗口主题切换 `mainwindow.cpp:606,708` 和页面切换 `slidingstackedwidget.cpp:135`）；预览缩略图 worker 默认关闭（`playerpreviewcontroller.cpp:89-90` 默认 `"time"`），且它用 `hwdec=no` + `MPV_RENDER_API_TYPE_SW` + 320×180 + 单线程（`mpvpreviewutils.cpp:96,103,120-125,135`），只吃 CPU 不吃 GPU；mpv 实例没有泄漏（单 PlayerWindow 复用 `playbackmanager.cpp:841-866`，warmup 实例在作用域内销毁 `mpvcontroller.cpp:84-99`）。

一句话：**别的客户端默认"按视频帧率播放"，qEmby 默认"按显示器刷新率渲染 + 每个渲染帧多 1~3 次全屏合成"**。在 24p→144Hz 下这是 6 倍渲染帧数再乘 2~3 次整窗拷贝，叠加起来就是用户看到的 3~6 倍 GPU 占用。

## 1. 对 Lead 线索的逐条复核

| # | 线索 | 复核结果 | 证据 |
|---|---|---|---|
| 1 | `windowutils.cpp:13` 在每个顶层窗口创建前塞隐藏 1×1 QOpenGLWidget 强制整窗 GL 合成；调用点 `mainwindow.cpp:97`、`playerwindow.cpp:13` | **成立** | `windowutils.cpp:13-39`（anchor 创建于 :31-36，注释 :37-39）；调用点确为 `mainwindow.cpp:97`、`playerwindow.cpp:13` |
| 2 | `main.cpp:31-39` 全局 GL 3.3 Core + DoubleBuffer + swapInterval=1；`AA_DontCreateNativeWidgetSiblings` | **成立**（行号实际 31-40） | `main.cpp:31-38` 设置 QSurfaceFormat；`:40` 设置 `AA_DontCreateNativeWidgetSiblings` |
| 3 | qEmby 默认 `video-sync=display-resample` 且 interpolation 强制 yes | **成立** | `mpvcontroller.cpp:145-149`；`pageplayer.cpp:84`；`playerdanmakucontroller.cpp:71-84` |
| 4 | JMP 默认 `video-sync=audio`（settings_description.json），全项目从不设 interpolation | **成立** | `/tmp/jmp/resources/settings/settings_description.json:369-372`（default audio）；`/tmp/jmp/src/player/PlayerComponent.cpp:1449` 只按设置设 video-sync；全项目 grep `interpolation` 0 命中 |
| 5 | JMP hwdec 默认为 auto（`PlayerComponent.cpp:1452-1465`），只有显式选 copy 才 auto-copy | **不成立，需纠正**：JMP 的**设置默认值是 `"copy"`**，映射为 `hwdec=auto-copy`；只有用户选 "Enabled" 才是 `auto` | `/tmp/jmp/resources/settings/settings_description.json:301-304`（`"default": "copy"`）；`PlayerComponent.cpp:1454-1465`（enabled→auto；copy→auto-copy） |
| 6 | mpv options.rst:1418-1419：d3d11va 需要 `--gpu-context=d3d11` 或 angle；qEmby 用 render API + gpu-api=opengl 只能用 *-copy | **成立**（本地副本行号 1246-1248） | `/tmp/options.rst:1246-1247`："d3d11va: requires --vo=gpu with --gpu-context=d3d11 or --gpu-context=angle"；`:1248`："d3d11va-copy: copies video back to system RAM" |
| 7 | 设置页只提供 auto-copy/d3d11va-copy/dxva2-copy/cuda-copy/no | **成立** | `pageplayer.cpp:57-61` |

> 第 5 条纠正很重要：如果报告把"copy-back"当成"只有 qEmby 高"的主因，会被 JMP 的默认配置直接反驳。**差异主因是 display-resample+interpolation 与 QWidget 合成架构**；copy-back 是两者共有的次级成本。

## 2. 量化模型

### 2.1 渲染帧率倍数（display-resample + interpolation 生效时）

条件：视频为恒定帧率（mpv display-* 模式要求 CFR），`interpolation-threshold` 默认 0.01（`/tmp/options.rst:5307-5327`），帧率/刷新率为整数或简单倍数时 display-sync 可正常工作（24→60 采用 2-3-2-3 交替，见 `--video-sync-max-video-change` 说明 `/tmp/options.rst:6839-6849`）。

| 片源帧率 | 60 Hz | 120 Hz | 144 Hz |
|---|---:|---:|---:|
| 23.976 fps | 2.50×（60 帧/秒） | 5.01×（120 帧/秒） | 6.01×（144 帧/秒） |
| 24 fps | 2.50× | 5.00× | 6.00× |
| 30 fps | 2.00× | 4.00× | 4.80× |
| 60 fps | 1.00×（阈值内不补帧） | 2.00× | 2.40× |

- 24p→60Hz：渲染 60 帧/秒，而 `video-sync=audio` 的参考实现只渲染 24 帧/秒。
- 24p→144Hz：渲染 144 帧/秒，是参考实现的 6 倍；且 `interpolation` 默认 `tscale=mitchell`（`/tmp/options.rst:5144-5148`），每个输出帧做一次多抽头时间轴卷积（Mitchell 半径 2，约 4 抽头），不是简单重复上一帧。
- 若显示器/片源比例超出容差，mpv 会自动回退到 `audio`（`/tmp/options.rst:6777-6780`）；所以实际倍数请以 qEmby 统计面板的 `display-fps / container-fps` 和日志 `videoRenders` 为准（见 §8）。

### 2.2 每个渲染帧的额外全屏 surface 操作次数

| 播放路径 | 每渲染帧额外整窗级操作 | 明细 |
|---|---:|---|
| 原生 swapchain（mpv `--wid` 子 HWND / `MPV_RENDER_API_TYPE_D3D11`） | **0** | mpv 直接 D3D11 呈现，Qt 不碰视频像素 |
| qEmby 普通路径（无弹幕） | **1 次** | QOpenGLWidget 的 FBO 纹理被顶层窗口合成进 backing store（`mpvwidget.cpp:359-368`；Qt 文档） |
| qEmby 弹幕 native-smooth（默认） | **3 次** | ① mpv 渲染进离屏 `m_videoFramebuffer`（`mpvwidget.cpp:513-540`）② 纹理 blit 到默认 FBO（`:560-577`）③ Qt 顶层窗口合成 |
| qEmby 弹幕 ASS 路径 | **1 次** | 无离屏 FBO，但 `video-sync` 被切到 `audio`、interpolation 关闭（`playerdanmakucontroller.cpp:834-835, 955`），渲染帧数先降到 24/秒 |
| JMP（Qt Quick `QQuickFramebufferObject`） | ~1 次纹理采样，但发生在 scene graph 同一渲染 pass 内，且只覆盖视频 item，不是整窗 backing store 合成 | `external/mpvqt/src/mpvabstractitem.cpp:25`（`QQuickFramebufferObject`）、`mpvrenderer.cpp:67-80`（渲染进 FBO，由 scene graph 当纹理采样） |

### 2.3 每帧字节数与额外带宽（按 1 次 = 读 1 帧 + 写 1 帧）

单帧 RGBA8 大小：1920×1080×4 = **8.29 MB**；3840×2160×4 = **33.18 MB**。

| 分辨率 | 刷新率 | 1 次整窗拷贝带宽 | 3 次（弹幕 native）带宽 |
|---|---:|---:|---:|
| 1080p | 60 Hz | 1.00 GB/s | 2.99 GB/s |
| 1080p | 120 Hz | 1.99 GB/s | 5.97 GB/s |
| 1080p | 144 Hz | 2.39 GB/s | 7.17 GB/s |
| 4K | 60 Hz | 3.98 GB/s | 11.94 GB/s |
| 4K | 120 Hz | 7.96 GB/s | 23.89 GB/s |
| 4K | 144 Hz | 9.56 GB/s | **28.67 GB/s** |

> 对照：双通道 DDR4-3200 的理论带宽约 51.2 GB/s，实际可用通常 35~45 GB/s。**4K/144Hz + 弹幕 native 时，仅 qEmby 自己的全屏合成就要 28.7 GB/s**，这足以把核显的显存/系统内存带宽打到饱和，任务管理器显示 GPU 3D/Copy 引擎接近满载。即使独显（300+ GB/s）不受带宽限制，432 次/秒的全屏 surface 操作（144 帧 × 3）本身也是实打实的着色器与 ROP 工作量。

### 2.4 解码回拷带宽（`hwdec=auto-copy`）

按 NV12 4:2:0 8bit 计算：1080p 单帧 3.11 MB，4K 单帧 12.44 MB；copy-back = GPU→RAM 回读 + RAM→GPU 上传 = 2 次全帧搬运。

| 分辨率 | 24 fps | 60 fps |
|---|---:|---:|
| 1080p | 0.15 GB/s | 0.37 GB/s |
| 4K | 0.60 GB/s | 1.49 GB/s |

这是"每解码帧"的固定成本，不随刷新率放大；在 4K 下与"每渲染帧"的合成成本同一量级，在 1080p 下相对次要。

### 2.5 组合示例（为什么看到 3~6 倍）

- **1080p 24p @ 60Hz，无弹幕**：参考实现 24 次 mpv 渲染/秒；qEmby 60 次 mpv 渲染/秒（2.5×）+ 60 次整窗合成（1.0 GB/s）。渲染类工作量约 2.5~3×。
- **1080p 24p @ 144Hz，无弹幕**：6× 渲染帧数 + 2.39 GB/s 合成带宽。
- **4K 24p @ 144Hz，弹幕 native**：6× 渲染帧数 × 3 次全屏 surface 操作 = 432 次/秒；合成带宽 28.7 GB/s；再加 0.6 GB/s 回拷。相对"24 次/秒 + 0 次合成"的参考实现，GPU 侧 surface 工作量放大约 **18 倍**（6×3），这解释了实测 3~6 倍（并非所有 GPU 工作都等比放大，固定开销和缓存命中会摊薄一部分）。
- **4K 24p @ 60Hz，弹幕 native**：2.5× 渲染 × 3 次 = 7.5× surface 量，11.9 GB/s。

## 3. 根因逐项（代码位置 + 机制）

### R1（最高优先级）默认 display-resample + interpolation=yes

- `mpvcontroller.cpp:145-149`：`ConfigStore::get<QString>(PlayerVideoSync, "display-resample")`，随后 `"interpolation" = (vsyncMode == "display-resample") ? "yes" : "no"`。**用户选 display-vdrop 时 interpolation 反而是关的**，只有默认的 display-resample 会开补帧。
- `pageplayer.cpp:76-84`：设置页第三项默认值 `QVariant("display-resample")`，选项只有 Audio / Display Resample (Default) / Display VSync。
- `playerdanmakucontroller.cpp:71-84`：运行时再次以 `"display-resample"` 为默认；`clearDanmakuMotionStabilityProfile()` 用它恢复（`:875-878`）。
- 后果：mpv 的 display-* 模式"完全同步于显示器刷新率"（`/tmp/options.rst:6767-6773`），`--interpolation` 沿时间轴卷积补出缺失帧（`:5295-5306`）。24p 在 144Hz 上就是 6 倍渲染。mpv 官方 issue 也直言在高刷新率屏上跑 display-resample "plain wasteful and taxing on the GPU"（[mpv#12005](https://github.com/mpv-player/mpv/issues/12005)）。
- **覆盖逻辑核查**：ASS 弹幕路径 `applyDanmakuMotionStabilityProfile()` 会把 video-sync 设为 audio、interpolation 设为 false（`:834-835`，调用点 `:955`）；**但默认的 native 弹幕路径在 `:933` 调用 `clearDanmakuMotionStabilityProfile()`，把默认的 display-resample+interpolation 又恢复回来**。也就是说"弹幕运动稳定性配置"只在非默认渲染器下生效，默认配置始终最贵。
- 触发条件补充：mpv 在检测到帧率无法匹配时会回退 audio（`/tmp/options.rst:6777-6780`）；VFR/直播等场景可能不生效，需实测确认（见 §8）。

### R2 每个渲染帧多 1 次整窗合成（QOpenGLWidget）

- `mpvwidget.cpp:23`：`MpvWidget : QOpenGLWidget`；`:28-31` `setSwapInterval(1)`。
- `mpvwidget.cpp:359-368`：`paintGL()` 取 `defaultFramebufferObject()`，把 mpv 渲染进 QOpenGLWidget 自己的 FBO。Qt 文档：paintGL 更新 widget 的 framebuffer，**"The widget's top-level window will then composite the framebuffer's texture with the rest of the window"**（[QOpenGLWidget](https://doc.qt.io/qt-6/qopenglwidget.html)）。
- `main.cpp:40`：`AA_DontCreateNativeWidgetSiblings` 禁止为原生 widget 的兄弟创建原生窗口 → 所有兄弟控件留在 backing store 合成路径；`main.cpp:31-38` 全局 Core 3.3 + 双缓冲 + swapInterval 1。
- `windowutils.cpp:13-39`：在每个顶层窗口（`mainwindow.cpp:97`、`playerwindow.cpp:13`）创建前塞一个隐藏的 1×1 QOpenGLWidget `playerCompositionAnchor`，注释写明目的是"prepared before native window creation"，即强制该窗口使用 OpenGL 合成（RasterGLSurface）。`playerwindow.cpp:34-39` 把 PlayerView 作为顶层 QWidget 的子控件；`playerview.cpp:1219-1224` MpvWidget 与 HUD/OSD/LoadingOverlay 等兄弟控件重叠。
- 后果：**窗口有多大，每帧就多 blit 多大**。独立播放窗口 1280×720（`playbackmanager.cpp:854`）每帧约 3.7 MB；4K 全屏 33.18 MB/帧；144Hz 即 9.56 GB/s 读+写。且这是"整窗"级操作，包含全部 QWidget UI 的合成，不只是视频矩形。
- 补充：默认播放模式是**主窗口内嵌**（`playbackmanager.cpp:175` `PlayerIndependentWindow` 默认 false → `:181-183` 走 embedded），MainWindow 也执行了 `prepareOpenGLComposition`（`mainwindow.cpp:97`），主窗口里有轮播、侧栏等大量控件，合成负担比独立窗口更大。

### R3（开弹幕时）离屏 FBO + TextureBlitter 二次合成 + 刷新率自驱动

- 默认渲染器：`danmakurendererutils.cpp:15-18` `defaultRendererId() = nativeSmoothRendererId()`；设置页 `pageplayer.cpp:558-568` 第一项即 Native Smooth Renderer，默认值同。
- `mpvwidget.cpp:343-353`：`paintGL()` 只要 overlay active 就进入 `paintNativeComposite()`。
- `mpvwidget.cpp:513-517`：离屏 `QOpenGLFramebufferObject`，`GL_RGBA8`、`NoAttachment`、尺寸 = 全窗口 `w×h`。
- `mpvwidget.cpp:529-540`：mpv 渲染进该 FBO（`BLOCK_FOR_TARGET_TIME=0`）。
- `mpvwidget.cpp:560-577`：`QOpenGLTextureBlitter` 把整张视频纹理 blit 回默认 FBO。
- `mpvwidget.cpp:585-586`：再 `QPainter` 画弹幕 sprite（`nativedanmakuoverlay.cpp:290-317` 逐个 `drawPixmap`）。
- `nativedanmakuoverlay.cpp:27-29`：`QOpenGLWidget::frameSwapped → requestNextFrame`，`:746-761` 每次 swap 后 `update()`；`:739-744` 按屏幕 `refreshRate()` 算间隔。**弹幕动画期间 widget 被自驱动到刷新率**，即使视频只有 24fps。
- `nativedanmakuoverlay.cpp:34-48`：还有 100ms 看门狗和 40ms 预取定时器（次要）。
- 量化：4K/144Hz 下每帧 3 次全屏操作 ≈ 28.7 GB/s（§2.3）；1080p/60Hz 也有 2.99 GB/s。**开弹幕与不开弹幕的 GPU 占用会有肉眼可见的台阶**，这也是用户最容易观察到的差异。

### R4 hwdec=auto-copy + Windows 强制 gpu-api=opengl

- `mpvcontroller.cpp:157`：默认 `"auto-copy"`；`:174` 写入。
- `mpvcontroller.cpp:159-161`：Windows 上无条件 `gpu-api=opengl`、`opengl-es=no`（仅 RDP 时降级 software，`:162-172`）。
- `pageplayer.cpp:57-61`：四个硬解选项全是 copy 变体；第一项标签写"Auto"但值是 `auto-copy`，用户无法在 UI 里选到零拷贝。
- 机制：`auto-copy` 只选"解码后把数据拷回系统内存"的模式（`/tmp/options.rst:1298-1303`），每帧 GPU→RAM→GPU 两次；`d3d11va` 零拷贝要求 `--vo=gpu` + `--gpu-context=d3d11 | angle`（`:1246-1247`），`d3d11va-copy` 明确"copies video back to system RAM"（`:1248`）。`gpu-api=opengl`（WGL/桌面 GL）与 D3D11 解码纹理无法原生互操作，所以只能走 copy。
- 量化：4K24 0.60 GB/s、4K60 1.49 GB/s、1080p24 0.15 GB/s（§2.4）。
- **对比校正**：JMP 默认也是 `auto-copy`（§1 第 5 条），所以本条不是"只有 qEmby 高"的原因；它解释的是 qEmby 在 4K 高码率下比浏览器/Emby Theater（真零拷贝）更高的那部分，以及为什么"开硬解"后 GPU Copy 引擎依然很忙。

### R5 GUI 线程在 paintGL 里被 mpv 阻塞等待（影响流畅度，间接放大帧成本）

- `mpvwidget.cpp:364-368`：普通路径的 `mpv_render_param` 只传 `OPENGL_FBO` 和 `FLIP_Y`，**没有传 `MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME`**。mpv render.h 明确该参数默认开启："the player attempts to render video a bit ahead, and then do a blocking wait until the target display time is reached. This blocks mpv_render_context_render() for up to the amount specified with the 'video-timing-offset' global option"（[render.h](https://github.com/mpv-player/mpv/blob/master/include/mpv/render.h)），而 `video-timing-offset` 默认 0.050s（`/tmp/options.rst:6740-6745`）。
- 对比：弹幕合成路径反而设置了 `blockForTarget=0`（`mpvwidget.cpp:531-535`）；JMP 直接设 `video-timing-offset=0`（`/tmp/jmp/src/player/PlayerComponent.cpp:144`，注释解释"render call runs on the Qt render thread... whole UI was capped to the video frame rate and the GUI thread spent most of its time waiting"）。
- 后果：QOpenGLWidget 的 paintGL 在 GUI 线程执行，display-resample 下每个 vsync 都可能阻塞等待 → GUI 线程被视频节奏绑定，HUD/弹幕调度抖动；不是直接 GPU 占用，但会迫使更多补帧/重绘，间接推高占用与卡顿。

### R6 HUD/控件的重绘与 QGraphicsEffect

- `playerview.cpp:1601-1609`：给 `m_topHUD`、`m_bottomHUD`、`m_logoLabel`、`m_networkSpeedLabel` 挂 `QGraphicsOpacityEffect`；`:1611-1628` 250ms 淡入淡出动画。
- `playerview.cpp:2512-2516`：右侧栏 `QGraphicsDropShadowEffect`（blurRadius 35）——默认隐藏（`:383 m_isRightSidebarVisible=false`），仅在打开侧栏时付费。
- `playerview.cpp:5613-5635`：`onPositionChanged` 每次 time-pos 回调都更新进度条 + 时间标签；`mpvcontroller.cpp:236` 观察 `time-pos`。`m_bottomHUD` 带 opacity effect，意味着进度条/标签每次重绘都要走"整个 bottomHUD 渲染到离屏 pixmap 再合成"的 QGraphicsEffect 路径。
- 量化：这条是 UI 侧固定成本，通常远小于 R1-R3，但在"弹幕/HUD 常显"时可感知；也是为什么暂停时 GPU 占用会明显下降（没有视频帧更新，但 HUD 动画/光标轮询仍在跑）。
- 其它定时器：`playerview.cpp:389-391` 10s 上报、`:399-413` 500ms buffer 查询、`:416-449` 100ms 鼠标轮询——都不是 60/144Hz 渲染源。

### R7 其它 mpv 选项（低优先级，但值得一起改）

- `mpvcontroller.cpp:132`：`blend-subtitles=video` —— 字幕被烘焙进视频帧，在 interpolation 开启时字幕也参与时间轴卷积（拖影），并让每帧多一次字幕混合；mpv 默认 `blend-subtitles=no`（`/tmp/options.rst:6610-6617`）。
- `mpvcontroller.cpp:133-139`：`sub-gray=yes`、`sub-blur=0`、`sub-gauss=0`、hinting none —— 单项成本小。
- `mpvcontroller.cpp:218-224`：只有用户开启 `PlayerUseMpvConf` 才加载 mpv.conf，且 `video-sync/hwdec` 是在 `mpv_initialize` 之前用 API 写入（等同命令行优先级，通常高于配置文件），所以**不要指望用户改 mpv.conf 覆盖这些默认值**，真正可靠的入口是 `pageplayer.cpp` 的两个下拉框。

## 4. 排除项与误报澄清（逐条核查结果）

| 嫌疑 | 结论 | 证据 |
|---|---|---|
| `grabFramebuffer` / `toImage` / `QWidget::grab` / `glReadPixels` 在渲染路径 | **不存在**。全仓库 grep：`grabFramebuffer` 0 次、`glReadPixels/readPixels` 0 次；`toImage` 只在图片加载/编辑（`imageutils.cpp`、`mediaservice.cpp`、`detailview.cpp`），`grab()` 只在主窗口主题切换（`mainwindow.cpp:606,708`）和页面切换（`slidingstackedwidget.cpp:135`） | 见 grep 结果；这些 `grab()` 不发生在播放循环里 |
| 预览缩略图 worker 持续解码、额外占 GPU | **默认不启用**（`PlayerHoverPreviewMode` 默认 `"time"`，只有 `"preview"` 才启用，`playerpreviewcontroller.cpp:89-90`）。且 worker 是 `hwdec=no` + `vd-lavc-threads=1` + `MPV_RENDER_API_TYPE_SW` + 320×180（`mpvpreviewutils.cpp:96,103,120-125,135`），只吃 CPU；即便开启也只在进度条悬停时按 80ms 防抖请求一帧 | 不构成 GPU 主因；若用户开了"预览"模式，会额外多一个软件解码 mpv 实例吃 CPU |
| 多个 mpv 实例并存 / 泄漏 | **不存在**：每个 PlayerView 一个 MpvWidget → 一个 MpvController（`mpvwidget.cpp:65`）；独立播放窗口被复用（`playbackmanager.cpp:841-866`，`WA_DeleteOnClose`）；warmup 只创建一次且临时对象在作用域结束销毁（`mpvcontroller.cpp:52-101`，`forceCleanup` `:36-50`） | 播放期间只有一个解码器 |
| 毛玻璃/模糊效果拖累播放 | **不在播放页**：`GlassBackdropWidget` 只被首页轮播使用（`mediacarouselwidget.cpp:172`），播放页无 blur effect | `playerview.cpp` grep blur 0 命中 |
| 进度条 60Hz 动画 | 进度条只在 `time-pos` 回调时重绘（约随视频帧/属性更新），不是独立 60Hz 定时器 | `playerview.cpp:5613-5635`；`mpvcontroller.cpp:236` |
| 播放页高频定时器 | 100ms 鼠标轮询、500ms buffer、10s 上报、250ms HUD 动画——均非逐帧渲染源；真正的逐帧源是 mpv 的 update 回调与弹幕 frameSwapped | `playerview.cpp:389-449`；`mpvwidget.cpp:212-221`；`nativedanmakuoverlay.cpp:27-29` |

## 5. 与其它客户端对比：为什么"只有 qEmby 高"

| 客户端 | 技术栈 / 视频表面 | 默认 video-sync / interpolation | 默认 hwdec | 每帧额外全屏合成 | 结果 |
|---|---|---|---|---|---|
| **qEmby** | Qt6 Widgets + libmpv OpenGL render API + **QOpenGLWidget**（`mpvwidget.cpp:23`） | `display-resample` + **interpolation=yes**（`mpvcontroller.cpp:145-149`） | `auto-copy`（`mpvcontroller.cpp:157`） | 普通 1 次；弹幕 native 3 次（`mpvwidget.cpp:513-577`） | **基准最高** |
| **Jellyfin Media Player / jellyfin-desktop** | Qt Quick + MpvQt `MpvVideoItem` = **QQuickFramebufferObject**（`external/mpvqt/src/mpvabstractitem.cpp:25`），mpv 在独立 worker 线程（`MpvController` moveToThread） | `audio`（settings 默认 `:369-372`；`PlayerComponent.cpp:1449`），**全项目不设 interpolation** | `copy` → `auto-copy`（settings `:301-304`；`PlayerComponent.cpp:1461-1463`）——**与 qEmby 相同** | 1 次纹理采样，发生在 scene graph 同一 pass，非整窗 backing store；`video-timing-offset=0`（`PlayerComponent.cpp:144`） | 低一大截：差别在同步默认值与合成路径，不在 hwdec |
| **Emby Theater（Electron）** | Electron/Chromium + 内嵌 libmpv 绑定（`libmpv/index.cc`），只设 `mpv_set_option_string(mpv_, "config", "yes")`（`:335`），其余 mpv 默认；PPAPI/Chromium GPU 合成 | mpv 默认 `audio` + `interpolation=no`（未显式设置） | mpv 默认（未启用硬解，除非 mpv.conf） | 无 QWidget FBO；由 Chromium GPU 进程合成/overlay | 低：按视频帧率、无补帧、无应用层整窗 blit |
| **浏览器网页版** | Chromium `<video>`：D3D11VA 硬解 → GPU 进程合成或 **hardware overlay 直接扫描输出**（零拷贝，[Khronos](https://www.khronos.org/vulkan/chrome-video/chromium_presentation_layer.html)） | N/A（Chromium 按视频帧率提交） | 零拷贝硬解 | 0（overlay 路径完全绕过 GPU 合成器） | 最低 |
| **mpv 命令行默认** | mpv 自身（vo=gpu/gpu-next 或平台默认） | `audio` + `interpolation=no`（`/tmp/options.rst:6800-6805, 5295-5306`） | `no`（默认不硬解，`:1238`） | 0（原生 swapchain） | 低 |

**"为什么只有 qEmby 高"可以归因成三条独立叠加的差异：**

1. **默认同步策略的差异（最大）**：只有 qEmby 把 `display-resample+interpolation` 当默认；JMP/Emby Theater/浏览器/mpv 默认都是按视频帧率出帧。24p→144Hz 是 6 倍渲染帧数，60Hz 也有 2.5 倍。
2. **视频像素穿过 Qt 合成器（qEmby 独有）**：JMP 虽然也用 OpenGL + `gpu-api=opengl`，但走 Qt Quick scene graph 的 `QQuickFramebufferObject`，纹理采样在同一 pass；qEmby 是 QWidget 体系 + `AA_DontCreateNativeWidgetSiblings` + 强制的隐藏 GL anchor，整窗 backing store 每帧都要合成。Emby Theater/浏览器则完全不经过 Qt。
3. **弹幕 native 渲染的二次合成（qEmby 独有）**：默认开启且默认 native-smooth，每帧多 1 次离屏写入 + 1 次纹理 blit；ASS 路径反而便宜（但被标注"Not Recommended"）。JMP 没有弹幕层，浏览器/Emby Theater 也没有这条。

补充：**hwdec=auto-copy 不是"只有 qEmby 高"的原因**（JMP 默认同样是 copy）；它解释的是 qEmby 在 4K/高码率下相对**浏览器/Emby Theater**（真零拷贝）多出的 0.15~1.5 GB/s 回拷，以及为什么硬解开着但 GPU Copy 引擎仍忙。

## 6. 修复建议（按收益排序）

### 6.1 用户现在就能改的（设置页，无需改代码）

| 序 | 操作 | 预期收益 | 代价 / 注意 |
|---|---|---|---|
| 1 | 设置 → Player → **Video Sync Mode = Audio** | 渲染帧率从 60/120/144 降回片源帧率（2.5× / 5× / 6× 的渲染量消失）；interpolation 自动变 no（`mpvcontroller.cpp:149` 由 mode 决定） | 24p 在 60Hz 上会有轻微 2-3 帧节奏抖动（这是 mpv 默认行为，JMP/Emby Theater 同样如此）；如果显示器支持 24Hz，可把桌面刷新率切到 24Hz 彻底消除 |
| 2 | **关闭弹幕**（或先关弹幕做 A/B） | 去掉每帧 2 次全屏 surface 操作 + 144Hz 自驱动重绘（R3）；4K144 场景可省 ~19 GB/s 合成带宽 | 没有弹幕；这是验证"弹幕是否为增量主因"的最快实验 |
| 3 | 设置 → 弹幕 → **Danmaku Renderer = ASS Subtitle Track (Not Recommended)** | 会走 `video-sync=audio` + interpolation=false（`playerdanmakucontroller.cpp:834-835, 955`），且没有离屏 FBO（R3 的 2 次拷贝消失） | 弹幕质量/样式变差（官方也标注不推荐）；但 GPU 会明显下降，适合低端机 |
| 4 | 设置 → Player → **独立播放窗口**（`PlayerIndependentWindow`，默认关闭，`playbackmanager.cpp:175`） | 减少主窗口其它 UI（轮播/侧栏）与视频同窗合成；独立窗口默认 1280×720 也比主窗口小 | 交互上多一个窗口；收益中等，不如 1/2 |
| 5 | 不要在 mpv.conf 里改这些 | —— | `config` 默认 no（`mpvcontroller.cpp:121`），且 API 选项优先；mpv.conf 适合调 `deband/dither` 等非关键项 |

> 推荐用户先做「1 + 2」：Video Sync = Audio + 关弹幕。这是零风险、可回退、收益最大的组合；如果必须保留弹幕，再加「3」。

### 6.2 代码修复（小改动、高收益，不改变产品形态）

1. **改默认同步策略（1 行级）**：`mpvcontroller.cpp:145-149` 默认 `video-sync=audio`；`interpolation` 只在用户显式选择 display-resample 时开。设置页把 "Display Resample (Default)" 改名为 "Smooth Motion (High GPU)" 并加说明（`pageplayer.cpp:78-84`）。同时把 `playerdanmakucontroller.cpp:71-84` 的默认值改成 audio。
2. **paintGL 不要阻塞 GUI 线程**：普通路径也传 `MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME=0`，并用 `MPV_RENDER_PARAM_NEXT_FRAME_INFO` 的 `target_time` 像 `paintNativeComposite()` 那样自己调度（`mpvwidget.cpp:464-508` 已有现成模板）；或至少设置 `video-timing-offset=0`。参考 JMP 的 `PlayerComponent.cpp:144` 注释。
3. **弹幕 overlay 与视频表面解耦**：不要让弹幕的 144Hz 重绘触发整个视频 widget 重绘。最彻底的是弹幕独立层（QQuickItem / 独立 QOpenGLWindow / layered native window），视频 widget 只在 mpv 新帧时重绘（音频同步模式下 24/秒）；过渡方案是把弹幕放到叠在视频上方的透明子控件里按刷新率重绘——这样每帧只多一次 Qt 合成，不重跑 mpv render，也没有离屏 FBO + blit。
4. **不要全局强制 GL 合成**：删除 `prepareOpenGLComposition()` 的 1×1 anchor（`windowutils.cpp:13-43`、`mainwindow.cpp:97`、`playerwindow.cpp:13`），去掉 `AA_DontCreateNativeWidgetSiblings`（`main.cpp:40`）。这不会让 QOpenGLWidget 变成零拷贝，但能避免"整个主窗口都由 GL backing store 合成"的额外开销，尤其是内嵌播放模式。
5. **HUD 性能**：`playerview.cpp:1601-1609` 的 `QGraphicsOpacityEffect` 改成自绘 alpha（`QPainter::setOpacity`）或在淡出结束后 `hide()`；时间和进度文本按 4~10Hz 节流，不要跟 `time-pos` 每个回调都刷新。
6. **设置页/文案**：`pageplayer.cpp:57` 的 "Auto" 实际是 `auto-copy`，应标注 "Auto (copy-back)"；在 Windows + QOpenGLWidget 架构下不要给用户"D3D11VA 零拷贝"的错觉（`pageplayer.cpp:58` 实际是 `d3d11va-copy`）。

### 6.3 架构修复（要拿"最低 GPU/零拷贝"必须做）

1. **视频像素不经过 Qt 合成器（Windows 首选）**：
   - 方案 A：`--wid` 子 HWND + `vo=gpu-next` + `gpu-api=d3d11` / `gpu-context=d3d11` + `hwdec=d3d11va`；mpv 自建 D3D11 swapchain，Qt 只负责 HUD（HUD 用独立透明窗口/overlay）。
   - 方案 B：`MPV_RENDER_API_TYPE_D3D11` + 自建 DXGI swapchain（`MPV_RENDER_PARAM_D3D11_FBO`），同 device 解码 → 零拷贝。
   - 收益：每帧额外全屏拷贝从 1~3 次降为 **0**；硬解从 copy-back 变零拷贝（4K 省 0.6~1.5 GB/s）。
2. **若必须单窗口 + Qt**：用 Qt Quick 的 `QQuickFramebufferObject`（JMP 方案）替代 QOpenGLWidget；渲染在 scene graph 渲染线程，纹理只作为 scene graph 的一个纹理节点，不再有"整窗 backing store 合成"；但仍比方案 A 多一次纹理采样。
3. **弹幕独立层**：弹幕作为独立 overlay 窗口/图层，视频层零重绘；弹幕层自己的刷新率与视频解耦（这是 R3 的根治）。
4. **可选：显示刷新率切换**：若产品想保留 24p 的 24Hz 原生节奏，可像 JMP 的 `DisplayComponent`（`refreshrate.auto_switch` 默认 false，`settings_description.json:285-289`）那样按片源切刷新率；否则 `video-sync=audio` 就足够，不需要 display-resample。

## 7. 给自研客户端的正确配置基线（mpv 选项清单）

### 7.1 目标形态：Windows 原生 D3D11 表面（推荐）

| 选项 | 推荐值 | 原因 |
|---|---|---|
| `vo` | `gpu-next`（或 `gpu`） | 显式指定，避免版本默认差异 |
| `gpu-api` | `d3d11` | 与 d3d11va 同一条 D3D11 路径，支持零拷贝 interop |
| `gpu-context` | `d3d11` | d3d11va 零拷贝的必要条件（`/tmp/options.rst:1246-1247`） |
| `hwdec` | `d3d11va`（验证后可加 `d3d11va-zero-copy=yes`） | 不写 `*-copy`；4K 下省 0.6~1.5 GB/s 回拷 |
| `video-sync` | `audio` | mpv 默认；按视频帧率出帧，最稳 |
| `interpolation` | `no`（显式） | 只在 `display-*` 下生效；开启会让 GPU 按刷新率补帧（`/tmp/options.rst:5295-5306`） |
| `blend-subtitles` | `no` | 避免字幕烘焙进视频帧；qEmby 现用 `video`（`mpvcontroller.cpp:132`） |
| `video-timing-offset` | `0`（render API 场景） | 避免 GUI/渲染线程在 `mpv_render_context_render` 内阻塞等待（默认 0.050s，`/tmp/options.rst:6740-6745`） |
| `osc` / `config` / `load-scripts` | `no` | 嵌入式播放器自带 UI，避免用户配置污染（qEmby 已做，`mpvcontroller.cpp:117-121`） |
| `keep-open` | `yes` | 播完停留最后一帧 |
| `cache` / `demuxer-max-bytes` | `yes` / `512MiB` | 网络流抗抖动（按内存预算调） |
| `hwdec-codecs` | `all`（可选） | 让更多 codec 尝试硬解；用 `hwdec-current` 验证 |

```ini
# 推荐基线（原生 D3D11 视频表面，Windows）
vo=gpu-next
gpu-api=d3d11
gpu-context=d3d11
hwdec=d3d11va
# d3d11va-zero-copy=yes      # 逐机型灰度，验证边缘伪影
video-sync=audio
interpolation=no
blend-subtitles=no
video-timing-offset=0        # 仅 render API 需要
osc=no
config=no
load-scripts=no
keep-open=yes
cache=yes
demuxer-max-bytes=512MiB
```

### 7.2 如果只能用 QOpenGLWidget / OpenGL render API（qEmby 现状的改良版）

此时 d3d11va 零拷贝不可用（`gpu-api=opengl` 与 D3D11 解码纹理无法互通），只能接受 copy-back；但必须做到：

```ini
vo=libmpv
gpu-api=opengl
hwdec=auto-copy              # 或 d3d11va-copy，接受回拷
video-sync=audio             # 关键：不要 display-resample
interpolation=no             # 关键：不要补帧
blend-subtitles=no
video-timing-offset=0
```

并配合 §6.2 的 paintGL 非阻塞 + 弹幕解耦 + 去掉强制整窗 GL 合成。**预期**：GPU 占用可回落到与 JMP 同档；但要低于浏览器/Emby Theater，仍差一次 FBO 合成和 copy-back。

### 7.3 验收指标（可直接做测试用例）

- `hwdec-current` 显示 `d3d11va`（不是 `d3d11va-copy`）；`current-vo` = `gpu-next`（或 `gpu`）。
- 播放 4K HEVC 10bit 60Mbps 10 分钟：任务管理器 **Video Decode** 引擎有负载、**3D** 引擎明显低于 qEmby 基线、**Copy** 引擎明显低于 auto-copy 基线。
- 统计面板 `display-fps` 与 `container-fps`：参考实现两者不必相等（audio 模式），但渲染次数应≈container-fps；qEmby 当前 display-resample 下会看到渲染次数≈display-fps（144/60），修复后应回落到 container-fps（24）。
- `frame-drop-count`/`decoder-frame-drop-count` 在 10 分钟内接近 0。

## 8. 现有代码里的验证手段（不用装新工具）

qEmby 自己已经打印了足够定位的数据，直接看日志/统计浮层即可验证：

| 数据 | 位置 | 用途 |
|---|---|---|
| `[MpvWidget] Video presentation ready` | `mpvwidget.cpp:54` | 确认首帧已呈现 |
| `[Danmaku][Composite] Cadence ... videoRenders/cachedFrames/deferredFrames/maxVideoRenderMs/maxCompositeCpuMs/maxCompositionMs/maxTargetDelayMs/vsyncVideoFrames` | `mpvwidget.cpp:607-614`（每 2s） | **直接量化"每 2 秒渲染了多少视频帧/缓存了多少帧"**；修复前后对比 videoRenders 应从 ~2×60/2×144 降到 ~2×24 |
| `[Danmaku][Native] Frame cadence | frames/late/maxGapMs/swaps/refreshHz/minStepMs/maxStepMs/maxPhaseErrorMs` | `nativedanmakuoverlay.cpp:340-350`（每 2s） | 确认弹幕层是否跑在刷新率、是否有掉帧 |
| 统计浮层：`Playback`（current-vo / hwdec-current / A-V）、`Refresh Rate`（display-fps / container-fps）、`Dropped frames` | `playerview.cpp:4754-4810` | 用户可直接截图对比 |
| 任务管理器 → 性能 → GPU → 引擎 | Windows | 看 3D / Copy / Video Decode 分项；PresentMon 可看每帧 present 间隔 |

**建议的 A/B 矩阵**：1080p / 4K × 60 / 144Hz × {弹幕关, native, ASS} × {display-resample, audio}，共 24 组；每组记录 10 分钟 GPU 3D/Copy 占用均值 + videoRenders/秒。预期：
- display-resample→audio：videoRenders/秒下降为 1/(刷新率/片源帧率)（60Hz 2.5×，144Hz 6×）。
- 弹幕 native→关闭：videoRenders 不变但 blit 次数从 3 次降为 1 次，GPU 3D 占用下降（4K 明显）。
- 弹幕 native→ASS：videoRenders 回落到 24/秒，GPU 进一步下降。

## 9. 关键证据索引（file:line 速查表）

| 结论 | 证据 |
|---|---|
| 默认 video-sync=display-resample | `mpvcontroller.cpp:145-147`；`pageplayer.cpp:84`；`playerdanmakucontroller.cpp:71-77` |
| interpolation 随 display-resample 强制 yes | `mpvcontroller.cpp:149`；`playerdanmakucontroller.cpp:80-84` |
| ASS 弹幕切换 audio/no-interp | `playerdanmakucontroller.cpp:834-835`（调用点 :955） |
| native 弹幕恢复 display-resample/no-interp→yes | `playerdanmakucontroller.cpp:933 → 875-878` |
| 默认 hwdec=auto-copy | `mpvcontroller.cpp:157, 174` |
| Windows 强制 gpu-api=opengl | `mpvcontroller.cpp:159-161` |
| 设置页硬解全是 copy | `pageplayer.cpp:57-61` |
| 设置页同步默认 display-resample | `pageplayer.cpp:76-84` |
| vo=libmpv | `mpvcontroller.cpp:125` |
| blend-subtitles=video | `mpvcontroller.cpp:132` |
| QOpenGLWidget 类型 | `mpvwidget.cpp:23` |
| paintGL 渲染进 defaultFBO | `mpvwidget.cpp:359-368` |
| 离屏 FBO（RGBA8） | `mpvwidget.cpp:513-519` |
| mpv 渲染进离屏 FBO（block=0） | `mpvwidget.cpp:529-540` |
| texture blit 回默认 FBO | `mpvwidget.cpp:560-577` |
| 弹幕 QPainter 绘制 | `mpvwidget.cpp:585-586`；`nativedanmakuoverlay.cpp:290-317` |
| 弹幕 frameSwapped 自驱动 | `nativedanmakuoverlay.cpp:27-29, 746-761` |
| 弹幕按屏幕刷新率 | `nativedanmakuoverlay.cpp:739-744` |
| 默认 native-smooth 渲染器 | `danmakurendererutils.cpp:15-18`；`pageplayer.cpp:558-568` |
| 全局 GL 3.3 + swapInterval 1 | `main.cpp:31-38` |
| AA_DontCreateNativeWidgetSiblings | `main.cpp:40` |
| 1×1 hidden QOpenGLWidget anchor | `windowutils.cpp:13-39`；调用 `mainwindow.cpp:97`、`playerwindow.cpp:13` |
| PlayerView/MpvWidget 层级 | `playerwindow.cpp:34-39`；`playerview.cpp:1219-1224` |
| HUD opacity effects | `playerview.cpp:1601-1609`；动画 :1611-1628 |
| 侧栏阴影（默认隐藏） | `playerview.cpp:2512-2516, 383` |
| time-pos 更新 UI | `mpvcontroller.cpp:236`；`playerview.cpp:5613-5635` |
| 100ms/500ms/10s 定时器 | `playerview.cpp:389-413, 416-449` |
| 预览 worker 默认关闭 + 软件解码 | `playerpreviewcontroller.cpp:89-90`；`mpvpreviewutils.cpp:96,103,120-125,135` |
| 单实例/窗口复用 | `playbackmanager.cpp:841-866`；`mpvwidget.cpp:65` |
| warmup 临时实例 | `mpvcontroller.cpp:52-101`（对象 :89-94 作用域销毁） |
| 内嵌模式默认 | `playbackmanager.cpp:175, 181-183` |

## 10. 外部依据

- mpv 手册（hwdec / video-sync / interpolation / video-timing-offset / tscale / blend-subtitles / video-sync-max-*）：https://mpv.io/manual/master/ ；本地副本 `/tmp/options.rst`（:1238, :1246-1248, :5144-5148, :5295-5306, :6610-6617, :6740-6745, :6760-6805, :6839-6849）
- mpv render API（BLOCK_FOR_TARGET_TIME 默认阻塞语义）：https://github.com/mpv-player/mpv/blob/master/include/mpv/render.h
- mpv issue #12005（display-resample 在高刷屏上"wasteful and taxing on the GPU"）：https://github.com/mpv-player/mpv/issues/12005
- Qt QOpenGLWidget（FBO + 顶层窗口合成）：https://doc.qt.io/qt-6/qopenglwidget.html
- Qt QQuickFramebufferObject（scene graph 纹理）：https://doc.qt.io/qt-6/qquickframebufferobject.html
- Jellyfin Media Player / jellyfin-desktop：https://github.com/jellyfin/jellyfin-desktop （本地 clone `/tmp/jmp`：settings_description.json:301-304/369-372，PlayerComponent.cpp:144, 1449-1465，MpvVideoItem.cpp:21-22）
- KDE MpvQt（QQuickFramebufferObject 实现）：https://invent.kde.org/libraries/mpvqt/-/raw/master/src/mpvabstractitem.cpp 、https://invent.kde.org/libraries/mpvqt/-/raw/master/src/mpvrenderer.cpp
- Emby Theater Electron（libmpv 绑定仅 config=yes）：https://github.com/MediaBrowser/emby-theater-electron （libmpv/index.cc:335）
- Chromium 硬件 overlay（零拷贝呈现）：https://www.khronos.org/vulkan/chrome-video/chromium_presentation_layer.html
