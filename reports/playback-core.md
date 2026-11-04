# 播放内核选型：有没有比 qEmby 更高效的方案？

> ⚠️ **2026-11 需求更新：必须支持 HDR + 杜比视界 + ASS 特效字幕。** 因为 libmpv 的 render API 不支持 gpu-next（mpv issue #10810），而 DV 必须靠 gpu-next/libplacebo，HDR 直通又要求 mpv 自己拥有 D3D11 swapchain，所以最终方案锁定为 **`--wid` + `gpu-next` + `gpu-api=d3d11`**；本文里"Qt Quick + render API"的方案仅适用于 SDR。详见 `decisions.md` v2。
>
> 结论先行：**引擎还是 libmpv 最好，但"接法"决定性能。** 同一颗 libmpv，接成 qEmby 那样（QOpenGLWidget 内嵌）就是最差的一档；接成 `--wid` 子窗口 + `vo=gpu-next` + `gpu-context=d3d11` + `hwdec=d3d11va` 就是目前 Windows 上接近最优的一档。换引擎（libVLC/FFmpeg 自研/Media Foundation）大多不会更快，只会在某些维度上更差或开发量大一个数量级。

---

## 一、先拆开看：播放内核的"成本"到底花在哪

播放一帧视频，成本分四段，**每段的优化空间完全不同**：

| 阶段 | 谁在做 | 能不能省 | 说明 |
|---|---|---|---|
| ① 解码（H.264/HEVC/AV1 → NV12/P010） | GPU 的固定功能解码单元（NVDEC / VCN / QSV / VideoToolbox） | **省不了** | 只要走硬解，各引擎调用的都是同一块硬件，功耗/占用基本一样 |
| ② 解码帧 → 渲染纹理 | 引擎 | **能省（关键）** | 零拷贝（d3d11va 直接采样解码表面）vs copy-back（GPU→内存→GPU）。qEmby 是后者 |
| ③ 渲染（YUV→RGB、缩放、HDR tone mapping、字幕混合） | 引擎的 shader | 部分能省 | 24fps 视频就该只跑 24 次；qEmby 默认跑 60/144 次（补帧） |
| ④ 呈现（swapchain → DWM → 屏幕） | 渲染后端 + 窗口系统 | **能省（关键）** | DXGI flip model 直出 ≫ 经 Qt/QWidget 合成器多一次 blit；hardware overlay 则是 0 次 GPU 合成 |

> 换句话说：**"哪个引擎"只影响 ②③④ 的实现质量；而"怎么接"决定 ②④ 有没有额外的全屏拷贝。** qEmby 的 GPU 高，90% 是 ③（补帧到刷新率）和 ④（QOpenGLWidget 整窗合成）造成的，不是 libmpv 的锅。

---

## 二、候选方案对比（2026-11 现状）

| 方案 | 解码 | 呈现路径 | 客户端 GPU | 直连播放覆盖 | HDR | 开发量 | 适用 |
|---|---|---|---|---|---|---|---|
| **libmpv + `--wid` 子 HWND**（推荐，今天就能用） | d3d11va（零回拷，可选 `d3d11va-zero-copy`） | mpv 自建 D3D11 swapchain（`d3d11-flip` 默认 yes，flip model） | **最低档**（≈ 仅解码 + 一次 shader） | 极好（FFmpeg 全家桶 + ASS 字幕 + 音轨直通） | 好（gpu-next + `target-colorspace-hint`） | 低 | Windows 桌面客户端首选 |
| **libmpv + `MPV_RENDER_API_TYPE_D3D11`** | d3d11va 零拷贝（同一 ID3D11Device） | 应用自建 DXGI swapchain | **最低档**，且单窗口 | 极好 | 好 | 中高 | 想要单窗口一体化的最终形态；**但 API 在 mpv 0.42（未发布稳定版）才有** |
| libmpv + OpenGL render API（qEmby 方案） | 只能 copy-back（拿不到 D3D11 device） | QOpenGLWidget FBO → 窗口合成 → DWM | 高 | 极好 | 一般 | 中 | 跨平台妥协；Windows 上不推荐 |
| libmpv + Vulkan render API / gpu-next | vulkan video（驱动支持少） | Vulkan swapchain（Windows 上仍走 DXGI） | 低~中 | 好 | 好 | 中 | Linux 优先；Windows 不占优 |
| **mpv 子进程 + IPC**（`mpv --wid=<HWND>`） | 同上 | 同上 | **最低档** | 极好 | 好 | 最低（先跑通） | 原型验证 / 不想自己链 libmpv |
| **libVLC 3.x** | d3d11va 零拷贝 | D3D11 vout（自有 swapchain） | 低~中 | 好（略逊 mpv） | 一般 | 低 | LGPL 商用友好；但 ASS 字幕/复杂音轨支持弱于 mpv。**libVLC 4 仍未发布（2026 只有 iOS beta 用预发布版）** |
| **FFmpeg + 自研 D3D11 渲染** | d3d11va 零拷贝 | 自建 DXGI swapchain | **理论最低**（完全可控） | 取决于你写多少 | 完全可控（可接 libplacebo） | **极高**（字幕/音轨/同步/seek 全自己写） | 有特殊需求（如自定义 HDR、专业色彩）才值得 |
| **DirectShow：LAV + MPC Video Renderer**（PotPlayer/MPC 那套） | DXVA2/D3D11 零拷贝 | MPC-VR 自建 D3D11 渲染器 | 低 | 很好 | 很好（HDR10/HLG/部分 DV） | 高（老框架、滤镜图复杂） | Windows 老牌播放器方案；做 Emby 客户端不划算 |
| **Windows Media Foundation（IMFMediaEngine）** | 系统解码 | EVR/DirectComposition | 低（可用 hardware overlay） | **差**（MKV 只支持 H.264/MPEG-2 等少数组合） | 取决于系统 | 中 | 不适合 Emby 这种混合媒体库，会大量触发服务端转码 |
| **Chromium / CEF / WebView2 HTML5 video** | 系统/内置硬解 | hardware overlay（DirectComposition，**GPU 合成 ≈ 0**） | **最低（几乎为 0）** | 差（HEVC/AV1/DTS/TrueHD 常需转码；ASS 字幕不可控） | 一般 | 低（UI 快） | 只适合"网页壳"；性能数字好看但代价是转码和格式限制 |
| Qt Multimedia（FFmpeg 后端） | 有硬解 | QVideoSink → 仍然是 Qt 渲染 | 中~高 | 中 | 弱 | 低 | 不推荐做严肃播放器 |

> 两条重要现状：
> * mpv 稳定版是 **0.41.0（2025-12）**；`MPV_RENDER_API_TYPE_D3D11` 在 **0.42.0（master，未发布）** 才加入（`DOCS/client-api-changes.rst`）。所以要用单窗口 D3D11 render API，得等稳定版或自己编。
> * VLC 4 / libVLC 4 到 2026 年底仍未正式发布，生产项目只能用 libVLC 3.x。

---

## 三、为什么"libmpv + --wid"在 Windows 上最快

1. **解码零回拷**：`hwdec=d3d11va`（配合 `gpu-context=d3d11`），解码出的 NV12/P010 纹理留在显存，直接被 shader 采样，不经过系统内存；qEmby 的 `auto-copy` 每帧要 GPU→RAM→GPU 搬两次。若想连 mpv 默认的那次 GPU→GPU 拷贝也省掉，可再开 `d3d11va-zero-copy=yes`（个别驱动有边缘伪影，需灰度）。
2. **呈现零额外拷贝**：mpv 自己拥有 D3D11 swapchain，`d3d11-flip`（默认 yes）走 DXGI flip model，DWM 直接合成，不需要"应用渲染到 FBO → Qt 再合一遍 → DWM"。
3. **渲染帧率 = 视频帧率**：`video-sync=audio`、`interpolation=no`，24fps 就只画 24 次。
4. **格式覆盖 = 不转码**：FFmpeg 能直读 MKV/HEVC/AV1/DTS/TrueHD，服务端就不需要转码——这才是整个系统层面最"省"的（转码吃的是服务器 CPU/GPU，而且画质会降）。

代价：`--wid` 是"mpv 自己画在一个子窗口里"，你的 Qt UI 无法盖在视频上（子 HWND 永远在父窗口客户区之上）。HUD/字幕/弹幕要么用 mpv 自己的 OSD，要么另开一个**分层透明窗口**盖在上面。这是这种方案唯一的架构成本，但完全可解（Plex/Emby 的不少第三方客户端都这么干）。

### 量级参考（4K HEVC 10bit 24fps，中端独显，144Hz 屏）

| 方案 | GPU 3D 引擎 | GPU Copy 引擎 | Video Decode 引擎 |
|---|---:|---:|---:|
| qEmby 默认（补帧 + QOpenGLWidget） | 30~60% | 5~15% | 20~35% |
| qEmby 只关补帧 | 10~20% | 5~15% | 20~35% |
| mpv `--wid` + d3d11va + flip | 3~8% | ≈0 | 20~35% |
| Chromium hardware overlay | ≈0~2% | ≈0 | 20~35% |

> 只是量级参考，具体随 GPU 型号、驱动、片源码率浮动很大；但"解码引擎占用各家一样、3D/Copy 引擎才是差异所在"这个结论是稳定的。

---

## 四、按你的场景选

**A. Windows 为主、要"播放最省 GPU + 开发别太夸张"**（最推荐）
```
Qt6 Widgets 或 QML 做壳
  └─ 子 HWND（QWindow::winId() / CreateWindowEx 的 child）
       └─ mpv 以 --wid=<HWND> 启动（同进程 libmpv 或独立 mpv.exe 都行）
            vo=gpu-next
            gpu-api=d3d11
            gpu-context=d3d11
            hwdec=d3d11va
            d3d11-flip=yes
            video-sync=audio
            interpolation=no
            blend-subtitles=no
            video-timing-offset=0     # 用 render API 时才需要
```
HUD 用一个独立的透明置顶窗口（`WS_EX_LAYERED | WS_EX_TRANSPARENT`，Qt 的 `Qt::Tool | Qt::FramelessWindowHint | Qt::WA_TranslucentBackground`）叠在视频上。

**B. 想单窗口、能接受用开发版 mpv**
用 `MPV_RENDER_API_TYPE_D3D11`（mpv 0.42+）：传自己的 `ID3D11Device`，mpv 渲染到你的 `ID3D11Texture2D`，你 Present 到自己的 swapchain。这是 qEmby 想达到而没达到的形态，也是 Plex 桌面端那类产品的做法。等 0.42 稳定后再上。

**C. 跨平台（Win + macOS + Linux）**
Windows 分支用 A；macOS/Linux 用 `MPV_RENDER_API_TYPE_OPENGL` + `QQuickFramebufferObject`（Jellyfin Media Player 的路线，Qt Quick 而不是 QWidget，能少一次整窗 blit）。不要用 QOpenGLWidget 嵌 QWidget。

**D. 只想先验证/不想碰 C++**
直接 `mpv --wid=<窗口句柄>` 子进程 + JSON IPC 控制，Windows 上用 `--input-ipc-server=\\.\pipe\xxx`。性能与 A 几乎一样，先把播放跑通再谈 UI。

---

## 五、什么时候"别的引擎"才真的更快

- **只看 GPU 数字、且只有 H.264/HEVC MP4**：Chromium/WebView2 的 hardware overlay 是物理上最省的（视频平面由显示控制器扫描输出，3D 引擎几乎 0）。但 Emby 库里只要出现 MKV/ASS/DTS/AV1，就会掉进转码或不能播——整体反而更慢。
- **需要极致 HDR/色彩管理**：FFmpeg + libplacebo 自研，或 DirectShow + MPC Video Renderer。代价是开发量。
- **需要 LGPL 闭源分发**：libVLC 3 是 LGPL；libmpv 默认 GPL，需用 `-Dgpl=false` 构建（功能会少一些，如部分音视频滤镜）。
- **其它情况**：libmpv 都是更优解。

---

## 六、验证方法（别信感觉，看引擎）

1. 任务管理器 → 性能 → GPU，引擎切到 **Video Decode / 3D / Copy**：
   * Video Decode 有负载、**Copy ≈ 0、3D 很低** = 零拷贝 + 正常呈现；
   * Copy 很高 = copy-back 解码；
   * 3D 很高 = 渲染帧率被拉高（补帧）或走了软件合成。
2. mpv 统计（`i` 或 `--osd-msg3`）：`hwdec-current` 应为 `d3d11va`（不是 `d3d11va-copy`）；`display-fps` 与 `container-fps` 不必相等，但渲染次数应 ≈ 视频帧率。
3. PresentMon 看 `Present()` 间隔：flip model 下应稳定在 16.7/6.9ms，且没有额外 blit。

---

## 六·补、`--wid` 子 HWND vs `MPV_RENDER_API_TYPE_D3D11` 到底差在哪

先把概念摆正：**这两者的区别不是"独立播放窗口 vs 单窗口播放"**，而是"**进程内谁来拥有视频表面（swapchain）、谁来 Present**"。

* "独立播放窗口 / 内嵌播放"是 **UI 层的产品决策**：播放器是弹出一个顶层窗口，还是嵌在主界面某个区域。
* "`--wid` / D3D11 render API"是 **渲染层的归属决策**。

两者正交：`--wid` 也完全可以嵌在主界面里（把主界面的一块区域做成 native child HWND 交给 mpv）；D3D11 render API 也完全可以做成一个独立的播放窗口。

| 维度 | `--wid` 子 HWND | `MPV_RENDER_API_TYPE_D3D11` |
|---|---|---|
| 进程内窗口数 | **2 个**：你的顶层窗口 + mpv 自己创建、以你给的 HWND 为 parent 的子窗口 | **1 个**：mpv 不建窗口 |
| 谁建 swapchain | **mpv**（内部 `CreateSwapChainForHwnd`） | **你**（自己 `CreateSwapChainForHwnd/Composition`） |
| 谁 Present | mpv | 你 |
| 视频帧是否经过你的代码 | **不经过**（只在 mpv 内部） | 经过：mpv 渲染进你提供的 `ID3D11Texture2D` |
| mpv 版本要求 | 任何版本 | **mpv 0.42+**（截至 2026-11 仍未发稳定版，只有 dev build） |
| 代码量 | ~10 行（传 HWND） | ~300~500 行（device / swapchain / resize / DPI / HDR / 渲染线程） |
| GPU 开销 | 一样低（都是 swapchain → DWM） | 一样低 |
| HUD 叠在视频上 | **难**：子 HWND 会盖住父窗口自绘内容，需要另开一个 `WS_EX_LAYERED` 透明窗口 | **易**：同一 device 上画，或用 DirectComposition 把视频/UI 叠成两个 visual |
| 弹幕与视频同帧合成 | 不行（除非改 mpv） | 可以（拿到纹理/渲染目标） |
| 自定义后处理、截图、缩略图 | 只能走 mpv 的 screenshot | 纹理直接可读，随便做 |
| 多路视频 / 画中画 | 多个子窗口，Z 序自己管 | 多个纹理 / 多个 visual，更灵活 |
| 输入事件 | 子窗口会吃掉鼠标/键盘，需要转发给 Qt 或禁用 mpv 输入 | 全部由你的窗口处理 |
| DPI 缩放、HDR 元数据、present 时机 | mpv 处理 | 你处理 |
| 窗口动画/拖拽/圆角阴影 | 子窗口体验较差，可能闪黑 | 完全可控 |

### 两个方案的最小骨架

```cpp
// 方案 1：--wid（今天就能跑）
WId child = createNativeChildWindow(parentWidget);      // 一个 WS_CHILD 的 HWND
mpv_set_option_string(mpv, "wid", QByteArray::number(quintptr(child)));
mpv_set_option_string(mpv, "vo", "gpu-next");
mpv_set_option_string(mpv, "gpu-api", "d3d11");
mpv_set_option_string(mpv, "gpu-context", "d3d11");
mpv_set_option_string(mpv, "hwdec", "d3d11va");
mpv_initialize(mpv);
// 之后只需要在 parent 的 resize 里 SetWindowPos(child, ...) 同步尺寸
```

```cpp
// 方案 2：D3D11 render API（mpv 0.42+）
ID3D11Device* dev = createD3D11Device();
mpv_render_context* ctx;
mpv_d3d11_init_params init{ dev, nullptr, 0 };
mpv_render_param params[] = {
    {MPV_RENDER_PARAM_API_TYPE,        (void*)MPV_RENDER_API_TYPE_D3D11},
    {MPV_RENDER_PARAM_D3D11_INIT_PARAMS, &init},
    {MPV_RENDER_PARAM_INVALID, nullptr},
};
mpv_render_context_create(&ctx, mpv, params);

// 每帧（由 mpv update callback 触发）：
mpv_d3d11_fbo fbo{ swapchainBackbufferTexture, w, h };
mpv_render_param rp[] = {
    {MPV_RENDER_PARAM_D3D11_FBO, &fbo},
    {MPV_RENDER_PARAM_INVALID, nullptr},
};
mpv_render_context_render(ctx, rp);
swapChain->Present(1, 0);
```

### 补充：render API 也需要一个"宿主"

容易误解的一点：`MPV_RENDER_API_TYPE_D3D11` 只解决"mpv 不建窗口、视频帧变成你的 D3D11 纹理"，**它不解决"纹理怎么显示出来"**。宿主有三条路：

| 宿主 | 能否真单窗口 | 交互/叠加 | 代价 |
|---|---|---|---|
| **Qt Quick**（`QQuickFramebufferObject` 或 `QQuickRhiItem`） | **能**：视频纹理作为 scene graph 的一个节点，QML UI 直接叠在上面 | 天然 | 需要 QML；OpenGL 路线要 copy-back 解码 |
| Qt Widgets（`QOpenGLWidget` 或 `QWindow + createWindowContainer`） | **不能**：D3D11 纹理要进 QWidget 仍得有一个原生子窗口 | 子窗口会挡住 QWidget | 又回到叠加/输入问题 |
| 自己接管顶层窗口的 D3D11 swapchain（+ DirectComposition 叠 UI） | 能 | 要自己写 | 工作量大，等于自己写一个极简合成器 |

所以对"UI 要叠在视频上 + 鼠标键盘要归 Qt"这个需求，**最顺的是 Qt Quick 这条路，不是 Widgets + render API**。

### 怎么选

1. **先做 MVP：用 `--wid`。** 真的零额外 GPU 开销，代码量只有 render API 的几十分之一，而且现在就能用。
2. **把接口抽象掉**：`class IVideoSurface { attach(mpv); resize(); setVisible(); HWND hwnd() or ID3D11Texture2D* } `，将来换 render API 只换实现，播放控制层不用动。
3. **什么时候必须升级到 D3D11 render API**：
   * HUD/弹幕必须**盖在视频画面之上**，又不想开第二个窗口；
   * 需要把视频和 UI 放进同一个 **DirectComposition** 视觉树（Chromium 那种做法，最干净）；
   * 需要自定义后处理、多路视频、画中画、逐帧截图；
   * 或者不想再受子窗口的输入/DPI/拖拽缺陷折磨。
4. 升级时机建议等 **mpv 0.42 正式发布**（或自己编一个 pin 住的 dev build）。

---

## 七、一句话总结

**没有比 libmpv 更"全能且高效"的播放内核；有比 qEmby 高效得多的接法。** 你要做的不是换引擎，而是别把视频像素丢进 Qt 的合成器，并且把补帧关掉。
