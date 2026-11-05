# qEmby 解码播放时 GPU 占用高的原因（代码取证 + 与自研方案对比）

> 分析对象：https://github.com/AlanHJ/qEmby ，本地 checkout `/root/github/emby-pc/qEmby`，HEAD `f551964`（与 upstream `main` 当前 HEAD 一致）。
> 方法：只读源码 + mpv 官方手册/源码 + Qt 官方文档 + 公开 issue；**未运行播放、未修改代码、未访问任何服务器**。
> 口径：窗口像素帧 = W×H（含 DPR），RGBA8 每像素 4B；1080p 帧 8.29 MB，4K 帧 33.18 MB；"一次全屏拷贝"= 读 1 帧 + 写 1 帧（2 帧字节）；"mpv 渲染写入"= 写 1 帧，单列不重复计入拷贝。
> 日期：2026-11-05

## 0. 结论（先看 8 条）

1. **不是解码器的锅**：硬解本身工作正常；高占用来自**渲染/合成次数被默认配置放大**，以及**视频像素被迫穿过 Qt 合成器**。暂停时 GPU 会明显回落、播放时升高，是"按帧渲染"型开销的典型特征。
2. **最大放大器：`video-sync=display-resample` + 强制 `interpolation=yes`**（`src/qEmbyApp/components/mpvcontroller.cpp:145-149`）。24fps 片源在 60/120/144Hz 屏上被渲染成 60/120/144 帧/秒 = **2.5× / 5× / 6×** 的渲染量，且每帧还多一次时间轴卷积（默认 `tscale=mitchell`）。
3. **第二层：每个渲染帧都会经过 QOpenGLWidget 的 FBO，再由 Qt 顶层窗口合成一次**（`mpvwidget.cpp:359-368` + Qt 文档）。即"视频帧 → widget FBO → 整窗 backing store → DWM"；4K/144Hz 这次额外拷贝约 **9.56 GB/s**。qEmby 还用 `AA_DontCreateNativeWidgetSiblings`（`main.cpp:40`）+ 每个顶层窗口预埋 1×1 隐藏 QOpenGLWidget（`windowutils.cpp:13-39`）把整窗强制为 GL 合成。
4. **第三层：render API 被锁死在旧渲染器 `vo_gpu`（gl_video），用不了 gpu-next/libplacebo**。mpv 源码中渲染 API 的后端表只有 `render_backend_gpu` 和 `render_backend_sw`（`video/out/vo_libmpv.c:114-117`），GPU 后端内部就是 `gl_video`（`video/out/gpu/libmpv_gpu.c:21,91`）。因此既没有 libplacebo 的新路径，也无法配合 D3D11 上下文做零拷贝/ HDR passthrough。
5. **第四层：硬解不是零拷贝**。Windows 上强制 `gpu-api=opengl`（`mpvcontroller.cpp:160`）、`hwdec=auto-copy`（`:157,174`）；`d3d11va` 零拷贝要求 `--gpu-context=d3d11` 或 `angle`（mpv 手册，本地 `/tmp/options.rst:1246-1248`），而 render API 用的是 Qt 提供的 WGL/桌面 GL 上下文（mpv `video/out/opengl/libmpv_gl.c:42,49`），只能 copy-back：每解码帧 **2 次全帧搬运**（4K24 约 0.60 GB/s、4K60 约 1.49 GB/s、4K10bit60 约 2.99 GB/s）。设置页四种硬解选项全是 `*-copy`（`pageplayer.cpp:57-61`）。
6. **额外固定成本：GUI 线程渲染 + mpv 阻塞等待 + vsync**。`paintGL` 在 GUI 线程调用 `mpv_render_context_render`，且普通路径没传 `BLOCK_FOR_TARGET_TIME=0`（`mpvwidget.cpp:364-368`），按 mpv render.h 默认会阻塞到目标显示时间（`video-timing-offset` 默认 0.050s）；只有弹幕合成分支传了 0（`:531-535`）。
7. **弹幕是可选叠加层，但默认渲染器（native-smooth）是最贵的一条**：先渲染到离屏 RGBA8 FBO，再整屏 blit 到默认 FBO，再 QPainter 画弹幕（`mpvwidget.cpp:513-540,560-577,585-586`），并由 `frameSwapped→update()` 自驱动到刷新率（`nativedanmakuoverlay.cpp:27-29,746-761`）。每帧额外 **2 次**全屏拷贝，4K/144Hz 约 **19.1 GB/s**。且 native 路径会把 `display-resample+interpolation` 恢复回来（`playerdanmakucontroller.cpp:933→875-878`），只有被标注"Not Recommended"的 ASS 路径才切到 `audio`（`:834-835,955`）。
8. **已排除的误报**：仓库**没有 Qt Quick/QML**（0 个 .qml、0 处 QQuick 引用），所以不存在"QML 场景图每帧重绘"；没有 `grabFramebuffer/toImage/glReadPixels` 回读；没有 OSD/deband/dither/ICC 每帧开销；没有多 mpv 实例；预览缩略图 worker 默认关闭且是软件解码；**不存在"把视频帧当纹理全量重传"的代码路径**（copy-back 的每解码帧上传是架构成本，不是每渲染帧重传）。

## 1. 研究范围与限制

- 代码：qEmby HEAD `f551964`。重点文件：`mpvcontroller.cpp`、`mpvwidget.cpp`、`playerview.cpp`、`playerdanmakucontroller.cpp`、`nativedanmakuoverlay.cpp`、`windowutils.cpp`、`playerwindow.cpp`、`main.cpp`、`pageplayer.cpp`。
- 对照：mpv 官方手册（本地副本 `/tmp/options.rst`）、mpv master 源码（`vo_libmpv.c`、`gpu/libmpv_gpu.c`、`opengl/libmpv_gl.c`、`gpu/video.c`）、Qt 6 文档；Jellyfin Media Player 本地 clone `/tmp/jmp`。
- 公开 issue：检索到 25 个 issue，搜 "GPU" 命中 3 条（#15 渲染引擎/DV、#4 动画流畅度、#3 全屏黑屏）；**没有直接的"GPU 占用高"反馈**。#15 作者确认 qEmby 是"OpenGL+UI 绘制"内置播放器，且外部强制指定 gpu-next 会弹出独立窗口、破坏内嵌——与源码结论一致。
- 限制：没有在目标机器上跑播放/抓 GPU 计数；本文的量化是"代码路径 × 分辨率/刷新率"的带宽与帧数推算，验证方法见 §8。

## 2. 现状播放链路（一张图）

```
HTTP/Emby 流
  └─ mpv demux/decode
       ├─ hwdec=auto-copy ⇒ D3D11VA 解码 → 回拷到系统内存(NV12/P010) → 上传 GL 纹理   ← 2 次全帧搬运/解码帧
       └─ vo=libmpv + render API(OPENGL) + vo_gpu(gl_video)
            └─ shader: YUV→RGB + 缩放 + (HDR 时 tone-map) + (interpolation 时时间轴卷积)
                 └─ 写入 QOpenGLWidget 的 FBO（RGBA8, 窗口像素）
                      └─ Qt 顶层窗口把该 FBO 纹理合成进 backing store（整窗，额外 1 次拷贝）
                           └─ DWM 呈现

弹幕开启（默认 native-smooth 时，额外多了）：
  视频 → 离屏 QOpenGLFramebufferObject(RGBA8) → QOpenGLTextureBlitter 整屏 blit 回默认 FBO → QPainter 画弹幕
        ↑ 由 QOpenGLWidget::frameSwapped → update() 自驱动到显示器刷新率（与视频帧率解耦）
```

## 3. 根因分解（代码证据 + 量化）

### 3.1 接入方式：libmpv render API（`vo=libmpv`），不是 `--wid`

- `mpvcontroller.cpp:125`：`mpv_set_option_string(m_mpv, "vo", "libmpv")`。
- `mpvwidget.cpp:298-309`：`mpv_render_context_create` 使用 `MPV_RENDER_API_TYPE_OPENGL`；渲染目标永远是应用提供的 FBO（`:359-368` 默认 FBO；`:529-537` 弹幕离屏 FBO）。
- `mpvwidget.cpp:23`：宿主是 `QOpenGLWidget`；Qt 侧 GL 上下文由 `QSurfaceFormat` 决定（`main.cpp:31-38`：3.3 Core / DoubleBuffer / swapInterval 1）。
- 全仓库**没有** `--wid`、没有原生子窗口、没有 `MPV_RENDER_API_TYPE_D3D11`（grep 0 命中）。
- 结论：视频像素的所有权在 Qt 手里——mpv 只是"被调用者"，没有自己的 swapchain，也没有 D3D11 上下文。

### 3.2 render API 锁死旧渲染器 `vo_gpu`（无 gpu-next/libplacebo）

这是结构性的，不是配置问题：

- mpv 源码 `video/out/vo_libmpv.c:114-117`：
  ```c
  const struct render_backend_fns *render_backends[] = {
      &render_backend_gpu,
      &render_backend_sw,
      NULL
  };
  ```
  渲染 API 只有 "GPU 后端" 和 "软件后端" 两种，没有 gpu-next 后端。
- `video/out/gpu/libmpv_gpu.c:21,91`：GPU 后端内部是 `struct gl_video *renderer`，`p->renderer = gl_video_init(...)`——即 **vo_gpu 的旧 `gl_video` 渲染器**，不是 libplacebo/gpu-next。
- 因此，即使新版本 mpv 把 gpu-next 设为默认 VO，`vo=libmpv` 的 render API 仍然走 `gl_video`。qEmby 想用 gpu-next 只能放弃 render API（改用 `--wid`/独立窗口）；render API 的 D3D11 类型只能拿到零拷贝，渲染器仍是 gl_video（见 §6.3）。
- 后果：
  1. 无法使用 `--gpu-context=d3d11` 的原生 D3D11 后端 → 不能与 D3D11VA 解码纹理互操作 → 只能 copy-back；
  2. 无法 `--target-colorspace-hint` 做 HDR passthrough（该选项要求 `--vo=gpu-next`，`/tmp/options.rst:6236-6239`）；
  3. 旧渲染器的中间 FBO 默认 `fbo-format=auto` → 优先 16F（`/tmp/options.rst:6187-6197`），配合 interpolation 会产生 16F 中间 pass（见 §3.7）；
  4. 这也能解释 issue #15：在外部 mpv.conf 强行指定 gpu-next，会脱离 render API 的内嵌路径，弹出独立窗口（作者已确认+qEmby 后续计划支持）。

### 3.3 硬解：`auto-copy`，不是零拷贝

- `mpvcontroller.cpp:157`：默认 `hwdec = "auto-copy"`；`:174` 写入。
- `mpvcontroller.cpp:159-161`（Windows）：无条件 `gpu-api=opengl`、`opengl-es=no`。render API 下 mpv 不创建上下文，用的是 Qt 的 WGL 桌面 GL 上下文（`libmpv_gl.c:34-50` 初始化一个 "blank ra_ctx"，`:42` 注释："vo_libmpv is essentially like a gigantic external swapchain where the user is in charge of presentation / swapping"）。
- mpv 手册：`d3d11va` 需要 `--vo=gpu` + `--gpu-context=d3d11` 或 `angle`（`/tmp/options.rst:1246-1247`）；`d3d11va-copy` 才是 "copies video back to system RAM"（`:1248`）。`--gpu-context` 列表见 `:6123-6165`（`win`=Win32/WGL，`d3d11`=原生 D3D11，`angle`=D3D11 经 ANGLE）。
- qEmby 设置页四种硬解：`auto-copy / d3d11va-copy / dxva2-copy / cuda-copy`（`pageplayer.cpp:57-61`），**没有任何零拷贝选项**；第一项标签叫 "Auto"，实际值是 `auto-copy`，容易误导。
- 帧格式路径：解码输出 NV12/P010（GPU）→ 回拷到系统内存（仍是 NV12/P010，mpv 不做 CPU RGBA 转换）→ 上传为 GL 平面纹理 → shader 做 YUV→RGB/缩放/（HDR）tone-map。所以**不存在"CPU 把每帧转成 RGBA"**；真实成本是两次全帧搬运 + Copy 引擎占用。

**copy-back 额外带宽**（每解码帧 2 次全帧搬运）：

| 分辨率 / 位深 | 单帧 | 24fps | 60fps |
|---|---:|---:|---:|
| 1080p NV12 | 3.11 MB | 0.15 GB/s | 0.37 GB/s |
| 1080p P010(10bit) | 6.22 MB | 0.30 GB/s | 0.75 GB/s |
| 4K NV12 | 12.44 MB | 0.60 GB/s | 1.49 GB/s |
| 4K P010(10bit) | 24.88 MB | 1.19 GB/s | 2.99 GB/s |

> 注意：这是"每解码帧"成本，不随显示器刷新率放大；所以它解释的是"4K/HDR 下比浏览器零拷贝更高"的那部分，而不是 6 倍渲染。

### 3.4 渲染帧率被放大 2.5× / 5× / 6×

- `mpvcontroller.cpp:145-149`：`video-sync` 默认 `display-resample`；`interpolation` 仅在 `display-resample` 时为 `yes`。
- 默认值在三处一致：`pageplayer.cpp:76-84`（设置页第三项 Default）、`playerdanmakucontroller.cpp:71-84`（运行期恢复默认）。
- mpv display-* 模式的语义：输出完全同步到显示器刷新率（`/tmp/options.rst:6767-6776`），需要 vsync-blocked 呈现（`:6792-6793`）；`interpolation` 要求 display-* 模式，否则被静默禁用，开启后用时间轴卷积补出缺失帧，`tscale` 默认 `mitchell`（`:5295-5306`、`:5144-5148`）。
- 结果（恒定帧率片源；display-sync 生效时）：

| 片源 | 60 Hz | 120 Hz | 144 Hz |
|---|---:|---:|---:|
| 23.976 fps | 2.50×（60 帧/s） | 5.01×（120 帧/s） | 6.01×（144 帧/s） |
| 24 fps | 2.50× | 5.00× | 6.00× |
| 30 fps | 2.00× | 4.00× | 4.80× |
| 60 fps | 1.00×（阈值内不补帧） | 2.00× | 2.40× |

- 旁证：mpv 官方 issue #12005 明确说在高刷屏上跑 display-resample "plain wasteful and taxing on the GPU"。
- **生效性提醒**：render API 不拥有窗口/swapchain，mpv 对显示器刷新率的检测/估算可能不准（`--override-display-fps` 默认用检测值，`/tmp/options.rst:1164-1172`；render API 下没有真实 swapchain），极端情况会回退 audio。验证方法见 §8：看统计面板 `display-fps`/`estimated-display-fps` 与日志里的 mpv 警告。

### 3.5 每个渲染帧多一次"整窗"Qt 合成

- `mpvwidget.cpp:336-380`：`paintGL()` 直接把 mpv 渲到 `defaultFramebufferObject()`。Qt 文档：`QOpenGLWidget` 先渲染进自己的 FBO，"the widget's top-level window will then composite the framebuffer's texture with the rest of the window"（[QOpenGLWidget](https://doc.qt.io/qt-6/qopenglwidget.html)）。
- 放大这次合成成本的两个设计：
  - `main.cpp:40`：`QGuiApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings)`——禁止给原生 widget 的兄弟创建原生窗口，全部走 backing store 合成；
  - `windowutils.cpp:13-39`：在每个顶层窗口创建前塞一个 1×1 隐藏 `QOpenGLWidget`（`playerCompositionAnchor`），注释写明 "prepared before native window creation"，即强制该窗口使用 GL 合成（RasterGLSurface）。调用点：`mainwindow.cpp:97`、`playerwindow.cpp:13`。
- 层级：`playerwindow.cpp:34-39` 把 `PlayerView` 放进顶层 QWidget；`playerview.cpp:1219-1224` 把 `MpvWidget` 与 HUD/OSD/LoadingOverlay 等兄弟控件叠在一起。**窗口多大，这次合成就是多大**（不是视频矩形）。
- 默认播放模式还是**主窗口内嵌**（`playbackmanager.cpp:175-183`：`PlayerIndependentWindow` 默认 false），MainWindow 同样做了强制 GL 合成（`mainwindow.cpp:97`），主窗口里还有轮播/侧栏等控件，合成压力比独立小窗更大。

### 3.6 弹幕开启时的额外两层（默认 native-smooth 是最贵路径）

- 默认渲染器：`danmakurendererutils.cpp:15-18` `defaultRendererId() = native-smooth`；设置页第一项即 Native Smooth Renderer（`pageplayer.cpp:558-568`）。
- `mpvwidget.cpp:343-353`：只要 overlay active，`paintGL` 进入 `paintNativeComposite`。
- `mpvwidget.cpp:513-519`：离屏 `QOpenGLFramebufferObject`，`GL_RGBA8`、`NoAttachment`、尺寸=全窗口。
- `mpvwidget.cpp:529-540`：mpv 渲染进该离屏 FBO（`BLOCK_FOR_TARGET_TIME=0`）。
- `mpvwidget.cpp:560-577`：`QOpenGLTextureBlitter` 整屏 blit 回默认 FBO；`:585-586` 再 `QPainter` 画弹幕 sprite（`nativedanmakuoverlay.cpp:290-317` 每个 `drawPixmap`）。
- `nativedanmakuoverlay.cpp:27-29`：`QOpenGLWidget::frameSwapped → requestNextFrame`，`:746-761` 每次 swap 后 `update()`；`:739-744` 按屏幕 `refreshRate()` 定时。**弹幕动画期间 widget 被自驱动到刷新率，即使视频只有 24fps**。
- 因此弹幕路径比普通路径每帧多 **2 次**全屏拷贝（blit + Qt 合成；离屏渲染写入是 mpv 渲染被改道，不额外计），4K/144Hz 额外约 **19.1 GB/s**。
- **关键讽刺**：native 路径在 `playerdanmakucontroller.cpp:933` 调 `clearDanmakuMotionStabilityProfile()`，把 `video-sync=display-resample`、`interpolation=yes` 恢复（`:875-878`）；而 ASS 路径在 `:834-835` 把 `video-sync=audio`、`interpolation=no` 打开（调用点 `:955`）。默认的 native 渲染器 = 高渲染率 + 多 2 次拷贝；"不推荐"的 ASS 渲染器反而最省 GPU。

### 3.7 每帧渲染相关选项盘点（qEmby 实际取值）

| 选项 | qEmby 取值 | 证据 | 每帧影响 |
|---|---|---|---|
| `vo` | `libmpv` | `mpvcontroller.cpp:125` | 决定走 render API，而非 `--wid` |
| `vo` 后端渲染器 | `gl_video`（vo_gpu），**不是 gpu-next** | mpv `vo_libmpv.c:114-117`、`libmpv_gpu.c:21,91` | 无 libplacebo；不支持 D3D11 上下文/HDR passthrough |
| `gpu-api` | `opengl`（Windows 强制） | `mpvcontroller.cpp:160` | WGL 桌面 GL；与 D3D11VA 无法互操作 |
| `gpu-context` | 未设置；render API 下由 Qt 提供上下文 | 全仓 grep 0；`libmpv_gl.c:34-50` | 不能变成 `d3d11` |
| `hwdec` | `auto-copy`（默认） | `mpvcontroller.cpp:157,174`；`pageplayer.cpp:57-61` | 每解码帧 2 次全帧搬运 |
| `video-sync` | `display-resample`（默认） | `mpvcontroller.cpp:145-147`；`pageplayer.cpp:78,84` | 按刷新率渲染（2.5~6×） |
| `interpolation` | display-resample 时 `yes` | `mpvcontroller.cpp:149` | 每刷新周期补帧 + 时间轴卷积 |
| `tscale` | 未设置（默认 `mitchell`） | `/tmp/options.rst:5144-5148` | 多抽头时间卷积 |
| `fbo-format` | 未设置（默认 `auto`，优先 rgba16f/rgba16） | `/tmp/options.rst:6187-6197` | 中间 pass 可能 16F（双倍带宽，见下） |
| `blend-subtitles` | `video` | `mpvcontroller.cpp:132` | 字幕烘焙进视频帧；随 interpolation 一起被时间过滤 |
| `sub-gray` | `yes` | `mpvcontroller.cpp:133` | 字幕转灰度（小成本） |
| `osc` | `no` | `mpvcontroller.cpp:117` | 无 OSC 脚本 |
| `osd-level` | 未设置（默认 1） | — | 无 OSD 内容，无每帧 OSD 重绘 |
| `deband` | 未设置（默认 no） | `/tmp/options.rst` | 无 |
| `dither-depth` | 未设置（默认 `no`） | `/tmp/options.rst:5349-5350` | 无抖动 |
| `icc-profile` / 色彩管理 | 未设置 | — | 无 ICC 3D LUT |
| `scale`/`cscale` | 未设置（默认随 libmpv 版本：手册称 bilinear，master 源码 `gl_video_opts_def` 为 lanczos） | — | 1080p→4K 放大成本因版本而异 |
| `tone-mapping` | 未设置（默认 auto） | `/tmp/options.rst:6362-6369` | 仅 HDR/广色域内容每帧生效 |
| `target-colorspace-hint` | 未设置（且需 gpu-next，本路径不可用） | `/tmp/options.rst:6236-6239` | 无 HDR passthrough，HDR 被 tone-map 到 SDR 8bit |
| `video-timing-offset` | 未设置（默认 0.050s） | `/tmp/options.rst:6740-6745` | `paintGL` 内可能阻塞等待 |
| `opengl-pbo` | 未设置（仅 RDP 时 `no`） | `mpvcontroller.cpp:170` | 上传路径默认 |
| `config` | `no`；用户可开（`PlayerUseMpvConf`） | `mpvcontroller.cpp:121,218-224` | 默认不读 mpv.conf |

**fbo-format 的隐藏成本（条件性）**：旧 `gl_video` 的中间纹理用 `p->fbo_format`（源码 `video/out/gpu/video.c`，`finish_pass_tex` 的 `ra_tex_resize(..., p->fbo_format)`），默认 `auto` 优先 16F。开启 interpolation 时会出现时间轴 merge 中间 pass，输出尺寸=窗口尺寸：

| 4K 中间纹理 | 单帧 | 读+写/帧 | 144Hz |
|---|---:|---:|---:|
| RGBA8 | 33.18 MB | 66.36 MB | 9.56 GB/s |
| RGBA16F | 66.36 MB | 132.7 MB | **19.1 GB/s** |

也就是说，**interpolation 在 4K/144Hz 下可能再叠加约 19 GB/s 的中间 FBO 带宽**（是否命中取决于驱动与 pass 数）。可用 `fbo-format=rgb10_a2`/`rgba8` 做 A/B 验证（仅对 vo_gpu/libmpv 路径生效；需开启 mpv.conf 或改代码）。

### 3.8 vsync / 阻塞 / pacing

- `main.cpp:36` 全局 `swapInterval=1`；`mpvwidget.cpp:28-31` 再次设置 `setSwapInterval(1)`。
- display-* 模式要求 vsync-blocked 呈现（`/tmp/options.rst:6792-6793`）；但这里的 swap 是 Qt 窗口的合成 swap，不是 mpv 自己的 swapchain。
- mpv render.h：`MPV_RENDER_PARAM_BLOCK_FOR_TARGET_TIME` 默认开启，"the player attempts to render video a bit ahead, and then do a blocking wait until the target display time is reached. This blocks `mpv_render_context_render()` for up to `video-timing-offset`"（[render.h](https://github.com/mpv-player/mpv/blob/master/include/mpv/render.h)）。
- qEmby：普通路径**没有**传该参数（`mpvwidget.cpp:364-368`）→ 默认阻塞；弹幕路径传了 0（`:531-535`）。再叠加 `video-timing-offset` 默认 0.050s，GUI 线程每个渲染帧可能被阻塞等待，造成 UI 卡顿、帧调度抖动；为了让 A/V 跟上，mpv 可能增加重复/丢帧，进一步放大渲染。
- display-resample 在 render API 下还依赖 display-fps 检测；qEmby 没有设置 `--override-display-fps`（`/tmp/options.rst:1164-1172`），如果检测不准会 pacing 错误、帧重复/丢帧增多。

### 3.9 Qt 侧开销（先纠正一个前提：没有 Qt Quick）

- **全仓库没有 Qt Quick/QML**：`find . -name '*.qml'`（排除 libs）0 个；`QQuick/QSG/scene graph` 在 `src/` grep 0 命中。视频承载是 **Qt Widgets 的 QOpenGLWidget**，所以不存在"QML 场景图每帧重绘"；对应的是 **QWidget backing store + QOpenGLWidget 合成**（§3.5）。
- GUI 线程渲染：`paintGL` 在 GUI 线程执行；`MpvController` 也创建在 GUI 线程（`mpvwidget.cpp:65`），事件与渲染都在 GUI 线程。没有独立渲染线程。
- 强制 GL 合成：`AA_DontCreateNativeWidgetSiblings` + 1×1 anchor（`main.cpp:40`、`windowutils.cpp:13-39`、`mainwindow.cpp:97`、`playerwindow.cpp:13`）让整个窗口走 GL backing store，视频 widget 每帧更新会触发整窗合成。
- HUD/overlay：`playerview.cpp:1601-1609` 给 topHUD/bottomHUD/logo/网速标签挂 `QGraphicsOpacityEffect`（250ms 淡入淡出，`:1611-1628`）；`time-pos` 每次回调都会更新进度条与时间标签（`mpvcontroller.cpp:236`、`playerview.cpp:5613-5635`）。HUD 可见时，bottomHUD 的 opacity effect 会把它整条渲染到离屏 pixmap 再合成，属于 CPU 侧固定成本（远小于 §3.4/§3.5，但会让"显 HUD 时更卡"）。
- `m_mpvWidget->setAutoFillBackground(true)`（`playerview.cpp:1220`）可能让 Qt 在 GL 绘制前额外填一次背景（小成本，可去掉验证）。
- 窗口/DPR：渲染目标 = `width()*devicePixelRatio()` × `height()*DPR`（`mpvwidget.cpp:339-340`）。4K 全屏 = 33.18 MB/帧 RGBA8；4K@150% 缩放面积 ×2.25 → 74.6 MB/帧，合成带宽同比放大。
- 无 CPU 回读：grep `grabFramebuffer/toImage/glReadPixels/readPixels` 在播放路径 0 命中；`QWidget::grab` 只出现在主窗口主题切换（`mainwindow.cpp:606,708`）和页面切换（`slidingstackedwidget.cpp:135`），不在播放循环。
- HDR：输出 FBO 是 RGBA8（弹幕 FBO 明确 `GL_RGBA8`，`mpvwidget.cpp:516`；QOpenGLWidget 默认也是 8bit 纹理），没有 10bit/HDR passthrough；HDR 内容每帧做 tone-map 到 SDR，且全屏/HDR 切换有已知黑屏 issue（#19、#3）。

### 3.10 公开 issue 旁证

- **#15**（DV Profile 5 绿屏）：作者确认 qEmby 是 OpenGL+UI 绘制；用户把 mpv.conf 改成 gpu-next 后视频变成独立窗口、与 UI 分离——说明 render API 路径与 gpu-next 互斥（与 §3.2 一致）。
- **#19**（Win11 播放触发 HDR 开关黑屏）、**#3**（全屏切换黑屏）：说明 render API + Qt 合成路径在 HDR/全屏切换上不成熟；这类问题与本报告的"视频像素穿过 Qt 合成器"同源，但不是 GPU 占用报告。
- 未检索到"GPU 占用高/风扇狂转"的公开 issue（搜索 GPU 仅命中上述 3 条），所以本文完全基于代码与文档。

## 4. 排除项（避免误诊）

| 嫌疑 | 结论 | 证据 |
|---|---|---|
| Qt Quick/QML 场景图每帧重绘 | **不存在**：无 .qml、无 QQuick 引用；是 Qt Widgets + QOpenGLWidget | `find *.qml` 0；`src/` grep 0 |
| CPU 回读（grabFramebuffer/toImage/glReadPixels） | **不存在于播放路径** | grep 0；`grab()` 仅在主题/页面切换 |
| 多个 mpv 实例并解码 | **不存在**：单 PlayerView→单 MpvWidget→单 MpvController；独立窗口复用；warmup 临时实例作用域销毁且不 loadfile | `mpvwidget.cpp:65`；`playbackmanager.cpp:841-866`；`mpvcontroller.cpp:52-101` |
| 预览缩略图 worker 额外解码 | **默认关闭**（`PlayerHoverPreviewMode` 默认 "time"），且 `hwdec=no` + SW render 320×180 + 单线程，只吃 CPU | `playerpreviewcontroller.cpp:89-90`；`mpvpreviewutils.cpp:93-105,120-125,135` |
| OSD 每帧重绘 | **无**：`osc=no`，没有每帧 OSD 命令/内容 | `mpvcontroller.cpp:117` |
| deband / dither / ICC | **默认关闭/未启用**：dither-depth 默认 no；无 deband/icc 设置 | `/tmp/options.rst:5349-5350` |
| 把视频帧当纹理"每渲染帧重传" | **不存在**：视频纹理只在新解码帧时上传；copy-back 是每解码帧一次 RAM→GPU，不是每渲染帧 | §3.3；`mpv_render_context_render` 复用已上传纹理 |
| 解码器本身异常 | 无证据：硬解正常配置（auto-copy）；高占用发生在渲染/合成侧 | §3.4-3.6 |

## 5. 量化对比：qEmby vs 我们的方案

我们的目标形态：**Qt Widgets + `--wid` 子 HWND + `vo=gpu-next` + `gpu-api=d3d11`/`gpu-context=d3d11` + `hwdec=d3d11va`（零拷贝）**。

### 5.1 每帧路径对比

| 维度 | qEmby（render API + QOpenGLWidget） | 我们的方案（--wid + gpu-next + D3D11） |
|---|---|---|
| 视频表面 | QOpenGLWidget 的 FBO（Qt 管理） | mpv 自建子 HWND + DXGI swapchain |
| 渲染器 | `gl_video`（vo_gpu 旧渲染器） | `gpu-next`（libplacebo） |
| GPU 上下文 | Qt WGL 桌面 GL 3.3 | D3D11 原生 |
| 硬解 | `auto-copy`：GPU→RAM→GPU，2 次全帧搬运/解码帧 | `d3d11va` 零拷贝（同 device 采样） |
| 每渲染帧额外全屏拷贝 | 普通 1 次（Qt 整窗合成）；弹幕 2 次（blit + Qt 合成） | **0** |
| 渲染帧率 | display-resample → 60/120/144 帧/秒（2.5/5/6×） | `video-sync=audio` → 24 帧/秒 |
| HDR | tone-map 到 SDR 8bit，无 passthrough | 可 10bit + `target-colorspace-hint=yes` passthrough（gpu-next） |
| 渲染线程 | Qt GUI 线程（`paintGL` 内阻塞等待） | mpv 自己的 VO 线程 |
| UI 合成 | Qt 整窗 backing store（强制 GL 合成） | HUD 独立 overlay，DWM 只合成小区域 |
| 已知风险 | GPU 高、全屏/HDR 黑屏（#3/#19）、DV 绿屏（#15） | --wid 的焦点/DPI/HDR/全屏切换需实测 |

### 5.2 额外 surface 流量（拷贝口径）

一次拷贝 = 读+写 1 帧。qEmby 普通 = 1 次/渲染帧；qEmby 弹幕 = 2 次/渲染帧；我们的方案 = 0。

| 分辨率 | 刷新率 | qEmby 普通（1 次） | qEmby 弹幕（2 次） | 我们的方案 |
|---|---:|---:|---:|---:|
| 1080p | 60 Hz | 0.99 GB/s | 1.99 GB/s | 0 |
| 1080p | 120 Hz | 1.99 GB/s | 3.98 GB/s | 0 |
| 1080p | 144 Hz | 2.39 GB/s | 4.78 GB/s | 0 |
| 4K | 60 Hz | 3.98 GB/s | 7.96 GB/s | 0 |
| 4K | 120 Hz | 7.96 GB/s | 15.93 GB/s | 0 |
| 4K | 144 Hz | **9.56 GB/s** | **19.11 GB/s** | 0 |

### 5.3 全屏 surface 操作次数（渲染写入 + 额外拷贝）

| 场景 | mpv 渲染/秒 | 额外拷贝/秒 | 合计 surface 操作/秒 | 相对方案（24 次/秒） |
|---|---:|---:|---:|---:|
| qEmby 1080p60 无弹幕 | 60 | 60 | 120 | 5.0× |
| qEmby 1080p60 弹幕 | 60 | 120 | 180 | 7.5× |
| qEmby 4K144 无弹幕 | 144 | 144 | 288 | 12.0× |
| qEmby 4K144 弹幕 | 144 | 288 | 432 | **18.0×** |
| 我们的方案 4K24 | 24 | 0 | 24 | 1.0× |

### 5.4 4K 24p 场景的"额外带宽"合计（拷贝 + copy-back）

| 场景 | 合成拷贝 | copy-back | 合计额外 |
|---|---:|---:|---:|
| qEmby 4K24 @60Hz 无弹幕 | 3.98 | 0.60 | **4.58 GB/s** |
| qEmby 4K24 @144Hz 无弹幕 | 9.56 | 0.60 | **10.16 GB/s** |
| qEmby 4K24 @144Hz 弹幕 | 19.11 | 0.60 | **19.71 GB/s** |
| qEmby 4K24 10bit @144Hz 无弹幕 | 9.56 | 1.19 | **10.75 GB/s** |
| 我们的方案 4K24 任意刷新率 | 0 | 0 | **0** |

> 口径说明：如果把"mpv 渲染写入"也按一次拷贝计入，会得到更大的数字（弹幕 4K144 约 23.9 GB/s 总 surface 流量，或按"3 次全屏操作"粗略估算会得到 ~28.7 GB/s）。本报告把渲染写入单列，避免与拷贝重复计算；无论哪种口径，量级都是"每秒十几~二十几 GB"，足以打满核显带宽并在独显上形成可观的 ROP/着色器负载。

### 5.5 结论性解释

- qEmby 的 GPU 高不是单点 bug，而是**默认配置 + 架构选择叠加**：`display-resample+interpolation` 把帧数放大 2.5~6×；`QOpenGLWidget` 再让每帧多一次整窗合成；`auto-copy` 让每解码帧多两次全帧搬运；`gl_video` 旧渲染器 + 可能的 16F 中间 FBO 让每帧更贵；弹幕 native 再叠加 2 次拷贝和 144Hz 自驱动。四项相乘，正好解释"GPU 占用高 3~6 倍"的观感（4K/144Hz 极端场景接近 18× surface 操作）。
- 我们的 `--wid + gpu-next + d3d11 + d3d11va` 方案把其中**三项直接归零**（Qt 合成拷贝、copy-back、display-rate 渲染），并让渲染回到 mpv 自己的线程/交换链；剩余成本只有 24fps 的 scale/tone-map（以及 HUD overlay 的小区域合成）。

## 6. 修复建议

### 6.1 用户现在就能改（qEmby 设置页，无需改代码）

| 序 | 操作 | 预期收益 | 代价/注意 |
|---|---|---|---|
| 1 | 设置 → Player → **Video Sync Mode = Audio** | 渲染帧率从 60/120/144 降回片源帧率（2.5×/5×/6× 消失）；interpolation 自动变 no（`mpvcontroller.cpp:149`） | 24p 在 60Hz 上会有 2-3 帧节奏；这是 mpv/JMP/浏览器的默认行为 |
| 2 | **关闭弹幕**（或先做 A/B） | 去掉每帧 2 次全屏拷贝 + 144Hz 自驱动重绘；4K144 省约 19 GB/s 拷贝带宽 | 无弹幕；验证增量主因最快的方法 |
| 3 | 弹幕渲染器改 **ASS Subtitle Track (Not Recommended)** | 会切 `video-sync=audio` + `interpolation=no`，且没有离屏 FBO（`playerdanmakucontroller.cpp:834-835,955`） | 弹幕效果/样式变差，但 GPU 明显下降 |
| 4 | 设置 → Player → **独立播放窗口** | 避免主窗口其它 UI 与视频同窗合成（`playbackmanager.cpp:175-183`） | 多一个窗口；收益中等 |
| 5 | 高级：开启 mpv.conf，显式写 `scale=bilinear`、`cscale=bilinear`、`fbo-format=rgb10_a2`（或 `rgba8`）、`dither-depth=no` | 消除版本默认 scaler 差异、把中间 FBO 从 16F 降到 8/10bit；可能省数 GB/s | 需要开启 `PlayerUseMpvConf`；代码通过 API 显式设置的选项（`vo`、`hwdec`、`video-sync`、`interpolation`、`gpu-api`、`blend-subtitles` 等）通常优先于 mpv.conf，别指望用它覆盖；mpv.conf 适合调代码未设置的 `scale/fbo-format/dither-depth` 等——真正可靠的开关是设置页 |

### 6.2 qEmby 代码级修复（按收益排序，若作者愿意）

1. **默认同步策略**：`mpvcontroller.cpp:145-149` 默认 `audio`，`interpolation=no`；把 display-resample 做成"平滑运动（高 GPU）"显式选项并加提示。这一项就能拿回大部分收益。
2. **放弃 render API 作为默认视频表面**：改用 `--wid` 子 HWND（mpv 自建窗口/D3D11 swapchain）或独立渲染窗口；HUD 用独立透明覆盖层。这是根治 Qt 合成拷贝的唯一办法。
3. **若必须保留 Qt 内嵌**：至少换成 `MPV_RENDER_API_TYPE_D3D11` + 自建 DXGI swapchain（零拷贝可行，但仍是 `gl_video`，没有 gpu-next），并去掉强制整窗 GL 合成（`windowutils.cpp` 的 1×1 anchor、`AA_DontCreateNativeWidgetSiblings`）。
4. **弹幕解耦**：弹幕不要触发视频 widget 重绘。视频 widget 只在 mpv 新帧时重绘；弹幕放到叠在上方的透明子控件/独立 overlay，按刷新率重绘。这样每帧最多多一次小区域合成，而不是 3 次全屏操作。
5. **不要阻塞 GUI 线程**：普通路径也传 `BLOCK_FOR_TARGET_TIME=0`，用 `NEXT_FRAME_INFO.target_time` 自己调度（弹幕分支已有模板，`mpvwidget.cpp:464-508`），或设 `video-timing-offset=0`。
6. **显式渲染质量/格式**：`fbo-format`、`scale`/`cscale`、`deband=no`、`dither-depth=no` 写死，避免版本默认差异；HDR 需要 gpu-next（见 2）。
7. **修正设置页误导**：`pageplayer.cpp:57` 的 "Auto" 实际是 `auto-copy`，应标 "Auto (copy-back)"；`pageplayer.cpp:58` 的 "D3D11VA" 实际是 `d3d11va-copy`。
8. **HDR/全屏**：8bit RGBA8 FBO + render API 无法 passthrough HDR；要在 gpu-next + 10bit swapchain 路径上才能解决（参考 issue #19/#3）。

### 6.3 我们方案的确认与修正

**结论：我们的 `Qt Widgets + --wid + gpu-next + d3d11 + d3d11va` 方向正确，且正好命中 qEmby 的四个根因；但有 3 个需要提前验证/设计决策的点：**

1. **--wid 是拿到 gpu-next 的唯一现实路径**：mpv 的 render API 没有 gpu-next 后端（`vo_libmpv.c:114-117`）；要同时要"gpu-next + 零拷贝 + 不经过 Qt"，必须让 mpv 拥有窗口/swapchain（`--wid` 子 HWND，mpv 自己创建子窗口，见 `/tmp/options.rst:3306`）或独立窗口。这一点与我们的方案一致。
2. **in-process 渲染 API 的取舍**：`MPV_RENDER_API_TYPE_D3D11` 可以做到零拷贝（`gpu/libmpv_gpu.c:10` 有 d3d11 context 后端），但渲染器仍是 `gl_video`——**拿不到 gpu-next/libplacebo**。如果团队坚持单窗口 + Qt 内嵌，需要接受"零拷贝但旧渲染器"；如果坚持 gpu-next，就必须 --wid。
3. **--wid 的风险清单**：Windows 上 `--wid` 是 mpv 创建自己的子窗口（`/tmp/options.rst:3306`），需要实测：焦点/键盘、HUD 透明覆盖层与命中测试、DPI 缩放、全屏/多显示器、HDR swapchain 切换、无边框拖动。建议在 MVP spike 里优先跑通这些，再做 UI。
4. **配置基线**：`vo=gpu-next`、`gpu-api=d3d11`、`gpu-context=d3d11`、`hwdec=d3d11va`（验证后加 `d3d11va-zero-copy=yes`）、`video-sync=audio`、`interpolation=no`、`blend-subtitles=no`、`osc=no`、`config=no`、`load-scripts=no`、`keep-open=yes`；HDR 时加 `target-colorspace-hint=yes` 并用 10bit swapchain。
5. **HUD 实现**：如果 HUD 用 Qt 透明窗口叠在 --wid 子窗口上，确认 DWM 合成区域只覆盖 HUD（不是整窗视频），且输入穿透正确；或者用 mpv OSD/独立 overlay。千万不要再让视频像素进入 Qt 的 backing store。

## 7. 为什么其他客户端低

| 客户端 | 视频路径 | 默认同步 | 默认硬解 | 额外全屏合成 | 结果 |
|---|---|---|---|---|---|
| **qEmby** | QOpenGLWidget + render API(OpenGL/vo_gpu) | display-resample + interpolation | auto-copy | 普通 1 次 / 弹幕 2 次 | 最高 |
| **Jellyfin Media Player** | Qt Quick `QQuickFramebufferObject`（MpvQt）+ render API | **audio**（settings 默认），全项目不设 interpolation | copy（settings 默认，同为 copy-back） | 1 次纹理采样（scene graph 同一 pass，非整窗） | 低 |
| **Emby Theater（Electron）** | Chromium + 内嵌 libmpv（只设 `config=yes`，其余 mpv 默认）/ HTML5 video | mpv 默认 audio / Chromium 视频帧率 | mpv 默认（未开硬解） | 无 QWidget FBO | 低 |

> 注：Emby Theater 的开源 Electron 壳（emby-theater-electron）中 `libmpv/index.cc:335` 只设置 `config=yes`，其余走 mpv 默认；新版/闭源实现可能有差异，但「Chromium 合成 + 视频帧率」这一结论不受影响。
| **浏览器网页版** | Chromium `<video>`：D3D11VA 解码 → GPU 进程合成/硬件 overlay 直出 | 视频帧率 | 零拷贝 | 0（overlay 路径） | 最低 |
| **mpv CLI** | 原生 swapchain | audio + interpolation=no | 默认 no（可开 d3d11va） | 0 | 低 |

关键差异只有三点（与 §5 对应）：**渲染帧率是否被锁到刷新率**、**视频像素是否穿过 Qt 合成器**、**硬解是否零拷贝**。qEmby 三项全中，所以只有它高。

## 8. 验证方法（不用装新工具）

qEmby 自己就打印了足够的数据；配合任务管理器即可验证本报告的每一条：

| 数据 | 位置 | 期望/用法 |
|---|---|---|
| 统计浮层 `Playback`（current-vo / hwdec-current / A-V）、`Refresh Rate`（display-fps / container-fps）、`Dropped frames` | `playerview.cpp:4754-4812` | 确认 `hwdec-current` 是否 `*-copy`；确认 display-fps 是否被错误检测；修复后 render 次数应≈container-fps |
| `[Danmaku][Composite] Cadence | videoRenders / cachedFrames / deferredFrames / maxVideoRenderMs / maxCompositionMs / maxTargetDelayMs / vsyncVideoFrames` | `mpvwidget.cpp:607-614`（每 2s） | 直接看每 2 秒渲染了多少视频帧；display-resample 下 4K144 应接近 288/2s，改 audio 后应接近 48/2s |
| `[Danmaku][Native] Frame cadence | frames / late / swaps / refreshHz / maxGapMs` | `nativedanmakuoverlay.cpp:340-352`（每 2s） | 确认弹幕层是否跑在刷新率、是否有掉帧 |
| 任务管理器 → 性能 → GPU → 引擎 | Windows | 看 3D / Copy / Video Decode 分项：3D 高=渲染/合成；Copy 高=copy-back；Video Decode=正常解码 |
| 对照实验 | — | ① Video Sync=Audio：3D 占用应显著下降（60Hz 约 2.5×，144Hz 约 6×）；② 关弹幕：4K 明显下降；③ 暂停：应回落到接近 0；④ 缩小窗口：合成带宽按面积下降 |

**验收基线（我们的方案）**：`hwdec-current=d3d11va`（不是 `d3d11va-copy`）、`current-vo` 为 `gpu-next`、4K HEVC 10bit 10 分钟：Video Decode 有负载、3D/Copy 明显低于 qEmby 基线、无 `*-copy` 字样、全屏/HDR 切换无黑屏。

## 9. 关键证据索引

| 结论 | 证据 |
|---|---|
| render API 接入（非 --wid） | `mpvcontroller.cpp:125`；`mpvwidget.cpp:298-309`；无 `--wid`/D3D11 render API（grep 0） |
| render API 只有 gl_video（无 gpu-next） | mpv `video/out/vo_libmpv.c:114-117`；`video/out/gpu/libmpv_gpu.c:21,91` |
| render API = 外部 swapchain，Qt 负责呈现 | mpv `video/out/opengl/libmpv_gl.c:42,49` |
| Windows 强制 gpu-api=opengl | `mpvcontroller.cpp:159-161` |
| hwdec=auto-copy 默认 | `mpvcontroller.cpp:157,174`；`pageplayer.cpp:57-61` |
| d3d11va 零拷贝需要 gpu-context=d3d11/angle | `/tmp/options.rst:1246-1248`；gpu-context 列表 `:6123-6165` |
| display-resample 默认 + interpolation=yes | `mpvcontroller.cpp:145-149`；`pageplayer.cpp:76-84`；`playerdanmakucontroller.cpp:71-84` |
| display-* 语义/要求 vsync-blocked | `/tmp/options.rst:6767-6776, 6792-6793` |
| interpolation 语义/tscale 默认 | `/tmp/options.rst:5295-5306, 5144-5148` |
| 每帧渲到 QOpenGLWidget FBO | `mpvwidget.cpp:336-380`（`:359-368`） |
| Qt 顶层窗口合成 QOpenGLWidget 纹理 | [QOpenGLWidget 文档](https://doc.qt.io/qt-6/qopenglwidget.html) |
| 禁止原生兄弟 + 强制 GL 合成 | `main.cpp:40`；`windowutils.cpp:13-39`；`mainwindow.cpp:97`；`playerwindow.cpp:13` |
| 弹幕离屏 FBO / blit / QPainter | `mpvwidget.cpp:513-519, 529-540, 560-577, 585-586` |
| 弹幕自驱动刷新率 | `nativedanmakuoverlay.cpp:27-29, 739-761`；`danmakurendererutils.cpp:15-18` |
| native 恢复 display-resample / ASS 切 audio | `playerdanmakucontroller.cpp:933→875-878`；`:834-835, 955` |
| fbo-format 默认 auto→16F | `/tmp/options.rst:6187-6197`；mpv `video/out/gpu/video.c`（`finish_pass_tex`/`fbo_format`） |
| 阻塞等待默认 | [render.h](https://github.com/mpv-player/mpv/blob/master/include/mpv/render.h)；`video-timing-offset` 默认 0.050：`/tmp/options.rst:6740-6745`；qEmby 普通路径未传 block 参数 `mpvwidget.cpp:364-368` |
| vsync=1 | `main.cpp:36`；`mpvwidget.cpp:28-31` |
| 无 Qt Quick/QML | 全仓 `*.qml` 0；`QQuick/QSG` 0 |
| 无回读 | grep `grabFramebuffer/toImage/glReadPixels` 0；`grab` 仅 `mainwindow.cpp:606,708`、`slidingstackedwidget.cpp:135` |
| 预览 worker 默认关闭/软件 | `playerpreviewcontroller.cpp:89-90`；`mpvpreviewutils.cpp:93-105,120-125,135` |
| 单实例/窗口复用 | `mpvwidget.cpp:65`；`playbackmanager.cpp:841-866` |
| 默认内嵌模式 | `playbackmanager.cpp:175-183` |
| HUD 视觉开销 | `playerview.cpp:1601-1609, 1611-1628`；`playerview.cpp:5613-5635`；`mpvcontroller.cpp:236` |
| 公开 issue 旁证 | #15（OpenGL+UI、gpu-next 会弹独立窗口）、#19（HDR 黑屏）、#3（全屏黑屏） |
| 8bit 输出/HDR | `mpvwidget.cpp:516`；无 `target-colorspace-hint`（grep 0） |

## 10. 参考

- mpv 手册：https://mpv.io/manual/master/ （本地副本 `/tmp/options.rst`，行号见正文）
- mpv render API：https://github.com/mpv-player/mpv/blob/master/include/mpv/render.h
- mpv 渲染后端源码：https://github.com/mpv-player/mpv/blob/master/video/out/vo_libmpv.c 、https://github.com/mpv-player/mpv/blob/master/video/out/gpu/libmpv_gpu.c 、https://github.com/mpv-player/mpv/blob/master/video/out/opengl/libmpv_gl.c 、https://github.com/mpv-player/mpv/blob/master/video/out/gpu/video.c
- mpv issue #12005（display-resample 在高刷屏上浪费 GPU）：https://github.com/mpv-player/mpv/issues/12005
- Qt QOpenGLWidget：https://doc.qt.io/qt-6/qopenglwidget.html
- Qt QQuickFramebufferObject：https://doc.qt.io/qt-6/qquickframebufferobject.html
- qEmby issues：#15 https://github.com/AlanHJ/qEmby/issues/15 、#19 https://github.com/AlanHJ/qEmby/issues/19 、#3 https://github.com/AlanHJ/qEmby/issues/3
- Jellyfin Media Player（对照）：https://github.com/jellyfin/jellyfin-desktop （本地 clone `/tmp/jmp`）
- Chromium 硬件 overlay：https://www.khronos.org/vulkan/chrome-video/chromium_presentation_layer.html
