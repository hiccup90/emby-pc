# 还缺什么：输入、决策、风险与里程碑

> 文档：reports/gaps.md · v0.1 · 2026-11-04 · 作者：planner（task-5）
> 依据：[decisions.md](decisions.md) v3、[architecture.md](architecture.md)、[playback-core.md](playback-core.md)、[qemby-analysis.md](qemby-analysis.md)、[app/README.md](../app/README.md)
> 一句话结论：**方案已定、骨架已有；现在缺的不是再调研，而是 9 项外部输入（其中 5 项硬阻塞）、6 个默认值决策、Windows 实机验证，以及 M1~M4 四段工程。** 本文件回答"还缺什么"，具体需求条目见 [docs/requirements.md](../docs/requirements.md)。

---

## 0. 现状盘点：有什么、缺什么

| 模块 | 现在有什么 | 还缺什么 | 归属 |
|---|---|---|---|
| 产品决策 | decisions v3 已确认：输出 SDR、不做 HDR 检测、控制条方案 A（mpv OSC）、mpv 0.41、`--wid` + gpu-next + d3d11 | 6 个小决策的默认值（见 B 节） | 本轮确定 |
| 架构 | architecture.md：分层图、播放选型、Emby API 调用序列、DWM 无边框方案、mpv 配置基线 | 评审后冻结 v1 | lead |
| 播放内核骨架 | app/：VideoHost（native child HWND）、MpvPlayer（动态加载 libmpv-2.dll）、OSC、键位、tone-map 配置、打开文件/文件夹 | Linux 编译验证（T1 进行中）、Windows 实机验证（M1） | T1 / M1 |
| Emby 接入 | 仅设计：认证、Items、PlaybackInfo、直连 URL、Sessions 上报 | 可编译的 API 客户端（T3 进行中）、真服务器联调（M2） | T3 / M2 |
| UI | mockups 控制条 A/B 对比；控制条选 A 已定 | 应用壳、媒体库、详情页、设置页（T4 进行中）；播放器 HUD 用 OSC | T4 / M3 |
| 打包 | 无 | 安装包、Qt/libmpv 依赖、版本信息、卸载/升级 | M4 |
| 测试 | 无测试素材、无性能基线 | A 节素材 + qEmby 基线 | 用户 |

**剩余工程量的粗略估算**（1 名全职开发 + 用户提供 Windows 机器）：M1 约 2 周、M2 约 2 周、M3 约 3 周、M4 约 1 周，合计 8~9 周；若只有业余时间，按 ×2 估。关键路径是 Windows 测试机到位时间，而不是代码量。

---

## A. 你必须提供的输入

### A-1 必需输入（IN-01~05 为硬阻塞）

| ID | 输入 | 硬性要求 | 用在 | 最晚需要 |
|---|---|---|---|---|
| IN-01 | **Windows 编译/测试机** | Windows 10 22H2 或 Windows 11 23H2 x64 实机（或可直通 GPU 的 VM）；GPU 支持 D3D11VA（NVIDIA GTX 1050+ / AMD RX 500+ / Intel UHD 620+ 级别即可）；8GB+ 内存；能访问 Emby 服务器；能安装 VS/Qt。**至少 1 台；推荐 2 台不同 GPU 厂商**（如 NVIDIA + Intel） | M1 起全部验收 | M1 开始前（本周） |
| IN-02 | **Windows 工具链** | Visual Studio 2022 Build Tools（Desktop development with C++、Windows 10/11 SDK 10.0.22621+）、Qt 6.8.x msvc2022_64、CMake ≥ 3.21、Git；若走 MinGW 则需对应 Qt MinGW 套件 | M1 编译 | M1 开始前 |
| IN-03 | **libmpv Windows 二进制** | mpv 0.41.0 x64 shared 构建：libmpv-2.dll + 匹配的头文件/导入库；必须含 **gpu-next / libplacebo**（DV 必需）与 FFmpeg 的 HEVC/DV 解复用；记录下载来源 URL 与 SHA-256。**需先定分发许可：默认 GPLv2+；若产品闭源，必须用 `-Dgpl=false` 的 LGPL 构建** | M1 播放、M4 分发 | M1 开始前 |
| IN-04 | **测试素材包** | 见 A-2 矩阵；素材需用户自有或有权用于测试 | M1 起全部播放验收 | M1 开始前 |
| IN-05 | **Emby 服务器与测试账号** | 服务器 URL（从 Windows 机器可访问）；测试账号/密码（建议临时账号，测完禁用）；产品与版本（Emby 4.8/4.9 或 Jellyfin 10.9/11/12）；测试库至少含 S1~S4 素材、1 部电影、1 部 ≥3 集且跨 2 季的剧；如为自签 HTTPS，提供证书指纹；测试用户需有播放权限 | M2 联调 | M1 期间提供 |
| IN-06 | **项目名称/品牌**（可先用占位） | 产品名、exe 名、公司名、图标、QSettings 的 organization/application 名、仓库地址。默认：产品名 "Emby PC"、exe "emby-pc"、私有仓库 | M3 标题栏/关于页、M4 安装包 | M3 前 |
| IN-07 | **分发要求**（M4 前） | 是否有代码签名证书；安装器偏好（Inno Setup / WiX / MSIX）；是否需要自动更新服务器；能否接受未签名内测版的 SmartScreen 警告。默认：Inno Setup、未签名内测、手动更新 | M4 | M4 前 |
| IN-08 | **目标硬件与显示基线** | 最低 GPU/CPU/内存；主用显示器分辨率/刷新率（decisions v3 的待确认项 2）。默认假设：1080p 60Hz SDR 显示器 + GTX 1650 级 GPU；参考机为 4K 60Hz | M1 性能验收 | M1 前 |
| IN-09 | **qEmby 基线数据**（强烈建议，非硬阻塞） | 在同一台 Windows 机器上安装 qEmby，用 S1 片源播放 10 分钟，记录任务管理器 GPU 的 3D/Copy/Video Decode 引擎峰值（或 PresentMon 数据）+ 截图，并注明 qEmby 设置 | 相对性能验收（3D 占用 ≤ qEmby 的 50%） | M1 末 |

> 安全提示：不要把真实密码直接发在聊天/日志里。建议建一个临时测试账号；如必须发密码，用后即改。客户端会把 token 存在本机设置中，P0 至少保证不进日志、不拼进 URL。

### A-2 测试素材矩阵

| ID | 类型 | 最低规格 | 用途 | 必需性 |
|---|---|---|---|---|
| S1 | HDR10 | 4K HEVC 10-bit，BT.2020/PQ，23.976 或 24fps，≥40Mbps，含暗部、高光、人物肤色场景 | 验证"不发灰"与 tone-map 正确性 | 必需 |
| S2 | 杜比视界 P5 | 4K，Profile 5（IPTPQc2），mkv/mp4 | 验证"不绿屏/不紫屏" | 必需 |
| S3 | 杜比视界 P8 | Profile 8.1（HDR10 基础层）与 8.4（HLG）各一 | 验证 P8 颜色正确 | 必需 |
| S4 | ASS 特效字幕 | 含 pos/move/t/fad/clip、卡拉 OK、嵌入字体、简繁中文 + 英文；至少 1 个真实番剧特效字幕，另可加自制测试文件 | 验证 ASS 样式与特效 | 必需 |
| S5 | SDR 基准 | H.264 或 HEVC，1080p，24fps，SDR | 回归基线、安装后冒烟 | 必需 |
| S6 | 多音轨 | TrueHD Atmos / DTS-HD MA / EAC3 5.1 各至少 1 条，含切换 | 音轨切换、音频直通（P1） | 建议 |
| S7 | 高码率长片 | 4K，≥60Mbps，≥2 小时 | 缓冲/长时稳定性/缓存设置 | 建议 |
| S8 | 外挂字幕 | 与 S5 同名的 .ass 与 .srt，附字体包 | sub-auto=fuzzy、外挂加载 | 建议 |

> 如果片库里没有 DV P5/P8 样片，可用公开测试片段，但必须确认来源与版权；S4 可以由项目自制（用 libass 全特性覆盖），但真实番剧特效字幕至少留 1 个，否则无法验证复杂排版。

---

## B. 尚未决定的小决策（给出建议默认值）

### B-1 六个必答项

| 编号 | 决策 | 候选 | **建议默认** | 理由/影响 | 最晚决定 |
|---|---|---|---|---|---|
| B-01 | **音频直通** | 全部直通 / 仅 AC3+ 直通 / 关闭 | **默认关闭（解码成 PCM）**；设置里提供"音频直通（AC3/EAC3/DTS/TrueHD）"开关，失败自动回退 PCM | 直通依赖功放/HDMI/驱动，配错会无声或爆音；MVP 先保证任何设备都有声 | M2 |
| B-02 | **字幕默认语言** | 简体优先 / 原语言优先 / 默认关闭 | **简中优先，其次繁中、英文；有字幕则默认开启**；建议 mpv `slang=chi,zho,zh,en,eng`，无匹配取第一条 | 中文用户；不改变用户手动选择 | M3 |
| B-03 | **上下集语义** | 剧集顺序 / 当前视图列表 / 只支持单集 | **剧集按 Season→Episode 升序**；从详情页"播放整剧"或"播放本季"生成队列；电影按当前列表顺序。**手动切集 P0；自动连播 P1（10 秒倒计时可取消）** | 符合剧集观看习惯；跨季自动衔接 | M2 |
| B-04 | **键位风格** | mpv 原生 / PotPlayer 风格 / 完全自定义 | **mpv 原生 + 现有骨架扩展**：空格暂停、←→ ±5s、Shift+←→ ±1s、↑↓ 音量、F 全屏、Esc 退出全屏、I 统计、N/P 上下集、双击全屏、滚轮音量 | 与 OSC 一致、零学习成本；详情见 requirements PLY-08 | M3 |
| B-05 | **窗口是否无边框** | 系统原生边框 / 无边框（原生 DWM 自绘）/ QWindowKit | **最终形态：无边框（只用原生 DWM API，禁止 QWindowKit）**；**M1~M2 先用系统标题栏**保证播放验证不受窗口问题干扰，**M3 切换 DWM 自绘标题栏** | 直接绕开 qEmby 的 1px 缝与 900×600 最小尺寸问题 | M3 |
| B-06 | **界面语言** | 简中 / 中英双语 / 跟随系统 | **MVP 简体中文单语**；所有字符串用 `tr()` 包裹，英文翻译 P1 | 降低 M3 工作量；不堵死国际化 | M3 |

### B-2 次要默认值（不阻塞，按此执行即可）

| 编号 | 决策 | **建议默认** | 说明 |
|---|---|---|---|
| B-07 | Jellyfin 兼容 | 代码从一开始就发标准 `Authorization: MediaBrowser ...` 头；**Emby 4.8/4.9 为 P0 验收，Jellyfin 10.9/11 为 P1 验收** | Jellyfin 12 已移除旧的 X-Emby-Token/api_key；多写几行头即可兼容，成本低 |
| B-08 | 续播与播放结束 | 服务端位置 >30 秒且 <95% 时**自动续播**；播放结束 `keep-open=yes` 停在最后一帧，OSD 提示"按 N 播放下一集" | 可在设置关闭自动续播 |
| B-09 | 多服务器 | **MVP 单服务器**；P1 再做多服务器/多用户切换 | 减少设置与 token 管理复杂度 |
| B-10 | 主题 | **仅深色主题**（取自 mockups 配色），不做浅色/跟随系统 | 播放器场景 + 节省 M3 时间 |
| B-11 | 更新与遥测 | **M4 手动下载安装包**；无遥测；崩溃日志本地保存，P1 再做可选上传 | 隐私优先、范围可控 |
| B-12 | 播放统计范围 | **P0 用 mpv stats（I 键）+ 进度上报服务端**；自绘统计面板放 P1 | mpv 自带 stats.lua 已覆盖分辨率/编码/丢帧/hwdec |
| B-13 | 日志 | 写入 `%LOCALAPPDATA%/emby-pc/logs`，5MB × 3 轮转，默认 INFO，设置可开 DEBUG；任何级别都不记录密码/token/Authorization | 排障必需，安全底线 |
| B-14 | 依赖/许可 | 应用代码许可由用户定；libmpv 按 IN-03 的 GPL/LGPL 决策执行 | 分发前必须解决 |

> **除 B-05（窗口形态）会影响 UI 设计、需要尽早确认外，其余都可先按建议默认推进，后续修改成本很低。**

---

## C. 技术风险与验证方式

### C-1 只能在 Windows 上验证的项

容器里没有 Windows、没有 GPU、没有音频设备，以下项**不能**在 Linux 容器里得出有效结论：

| 验证项 | 容器里为什么测不了 | 正确验证方式 | 失败判据 | 阶段 |
|---|---|---|---|---|
| D3D11VA 零拷贝 | 无 GPU/Windows | mpv stats 看 `hwdec-current`；任务管理器 GPU 的 Copy 引擎 | 出现 `d3d11va-copy` 或 Copy 引擎明显负载 | M1 |
| HDR10/DV tone-map | 无 HDR 显示链路/GPU | S1~S3 实播 + 与同机 mpv CLI 参考截图对比 | 发灰、绿屏、紫屏、偏色 | M1 |
| `--wid` 子窗口 | Win32 API 不存在 | 缩放/全屏/切屏/点击 OSC；WinSpy++ 看窗口样式 | 黑屏、焦点丢失、尺寸不同步 | M1 |
| OSC 交互 | 同上 | 鼠标悬停/点击各按钮、键盘快捷键、DPI 100~200% | 按钮无效、布局错位 | M1/M3 |
| DWM 无边框 | 无 DWM | 最大化/贴边/分屏截图取最外 1px | 有 1px 缝、Snap 失效 | M3 |
| GPU 占用 | 无 GPU | 任务管理器 GPU 引擎 + PresentMon | 3D 占用 > qEmby 基线的 50% | M1 |
| 音频输出/直通 | 无音频设备 | 耳机/功放实测，多声道片源 | 无声、爆音、音画不同步 | M2/P1 |
| 安装包 | 无 Windows | 干净 Win10/11 VM 安装、启动、播放 | 缺 DLL、启动失败 | M4 |
| Emby 真服务器联调 | 容器无账号/无库 | 真实服务器 + Dashboard + 客户端日志 | 登录失败、进度不上报 | M2 |

### C-2 风险登记表

| ID | 风险 | 触发条件/表现 | 概率 | 影响 | 验证与缓解 | 阶段 |
|---|---|---|---|---|---|---|
| R-01 | **没有 Windows 测试机**，所有渲染结论无法闭环 | 只能编代码，无法验证 HDR/DV/GPU/窗口 | 极高 | 阻塞 | 用户提供实机或远程桌面；M1 前必须落实 | M1 前 |
| R-02 | DV P5/P8 仍绿屏/偏色 | 特定 profile、特定 GPU 驱动、libplacebo 版本不匹配 | 中 | 高 | S2/S3 全播；记录 mpv/libplacebo 版本；必要时换 mpv 官方 build 或调整 `vo=gpu-next` 配置；仍失败则明确告知"该片源不支持"而不是绿屏 | M1 |
| R-03 | HDR10 发灰（Windows HDR 模式） | Windows 开着 HDR、SDR 输出被系统二次映射 | 中 | 高 | HDR 开/关两种模式各测；配置 `target-trc=srgb`、`target-prim=bt.709`、`target-colorspace-hint=no`；文档建议关闭 Windows HDR | M1 |
| R-04 | `--wid` 子窗口问题 | 黑屏、闪烁、焦点抢不到、DPI 放大后视频错位、全屏切换闪黑 | 中 | 高 | M1 最早做原型；至少 2 种 DPI、2 种分辨率、全屏进出 20 次；必要时改用独立播放窗口或等 mpv 0.42 的 D3D11 render API | M1 |
| R-05 | mpv OSC 功能不够 | 没有真正的下拉菜单/复杂交互；选轨体验差 | 高 | 中 | 验证 mpv select.lua 与自写 Lua 菜单；关键操作（字幕/音轨选择）必须可用；不足则 P1 用 Qt 透明覆盖窗（decisions 中方案 B 作为后手） | M1/M3 |
| R-06 | 性能目标不达标 | GPU 3D 高、丢帧、24p 在 60Hz 上抖动 | 中 | 高 | 同机 qEmby 基线 + PresentMon；确认 `video-sync=audio`、`interpolation=no`、未误开补帧；检查 mpv 是否真的用了 gpu-next/d3d11 | M1 |
| R-07 | Emby/Jellyfin 认证差异 | Jellyfin 12 用旧头 401；设备列表重复 | 中 | 中 | Emby 4.8/4.9 + Jellyfin 10.9/11/12 全跑；DeviceId 持久化；401 自动跳登录 | M2 |
| R-08 | 直连率低/被迫转码 | DeviceProfile 不全或服务端策略导致转码 | 中 | 中 | 记录 PlaybackInfo 响应与 Dashboard 的 PlayMethod；补 DirectPlayProfile；P1 做 HLS 转码 fallback | M2 |
| R-09 | 高码率网络卡顿 | 4K 60Mbps 走 Wi-Fi 时缓冲不足 | 中 | 中 | 有线/Wi-Fi 对比；`cache=yes`、`demuxer-max-bytes=512MiB`；缓冲状态可观察 | M1/M2 |
| R-10 | ASS 特效性能/兼容 | 复杂卡拉 OK、遮罩特效掉帧或错位 | 中 | 中 | S4 播放记录 `frame-drop-count`；`sub-ass-override=no`、`embeddedfonts=yes`；如 GPU 开销过高，复核 `blend-subtitles=video` 的取舍（decisions v3 已定，变更需记录） | M1 |
| R-11 | libmpv 分发许可违规 | 闭源分发 GPL 构建 | 中 | 高 | 走 `-Dgpl=false` LGPL 构建、动态链接、保留重链接权利；M4 前让法务/用户确认 | M4 前 |
| R-12 | 打包缺依赖 | 干净机缺 Qt 插件/VC 运行库/libmpv DLL | 中 | 高 | windeployqt + 手工补 libmpv-2.dll；干净 Win10/11 VM 实测 | M4 |
| R-13 | 长时稳定性/内存泄漏 | 8 小时播放崩溃、内存持续增长、切集卡死 | 中 | 高 | 8h soak、100 次快速切集、Application Verifier/VMMap；退出后检查无残留进程 | M3/M4 |
| R-14 | token 安全 | 日志/URL/崩溃转储泄露 token；QSettings 明文 | 低 | 高 | token 只走请求头；日志脱敏；QSettings ACL；P1 用 DPAPI 加密 | M2/M3 |
| R-15 | 音频直通兼容性 | 功放不支持、切轨无声或爆音 | 低 | 中 | 默认关闭；开启后失败自动回退 PCM；多设备实测 | P1 |
| R-16 | SmartScreen 拦截 | 未签名 exe 被警告/拦截 | 高 | 低 | 内测说明文档；有证书则签名 | M4 |

### C-3 验证工具与证据要求

| 工具/入口 | 用途 | 记录方式 |
|---|---|---|
| mpv stats（I 键）或 `--osd-msg3` | 看 `hwdec-current`、`current-vo`、`container-fps`、`display-fps`、`frame-drop-count` | 截图 + 文本日志 |
| Windows 任务管理器 → 性能 → GPU（引擎切 Video Decode/3D/Copy） | 观察解码/渲染/回拷占用 | 截图（峰值） |
| PresentMon | Present 间隔、是否 flip model、是否有额外 blit | CSV/截图 |
| 屏幕截图 + 像素检查（Python/PIL 或 ImageMagick） | 窗口贴边 1px 缝、HDR 颜色对比 | PNG + 结论 |
| Emby 服务端 Dashboard | 会话、PlayMethod、进度、设备 | 截图 |
| WinSpy++ / Window Detective | 窗口样式、客户区/窗口矩形、DPI | 截图 |
| Process Explorer / VMMap | 内存、句柄、线程泄漏 | 数值表 |
| 浏览器/curl + 抓包（Wireshark/Fiddler） | 检查 URL 不含 token、请求头正确 | 请求样本 |

> 每个里程碑的测试报告建议放在 `docs/test-reports/M1-<date>.md` 等位置，包含：机器配置、Windows 版本、GPU/驱动、mpv/libmpv 版本、片源、操作步骤、数值、截图、结论、已知问题。至少 M1 开始前建好这个目录。

---

## D. 里程碑 M1~M4 与退出标准

> 估算前提：1 名全职开发 + 用户在 Windows 机器上配合验证；Windows 机器不能是 WSL/虚拟机（无 GPU 直通时部分项不可测）。每个阶段结束由 lead 按 [docs/requirements.md](../docs/requirements.md) 对应条目验收。

| 阶段 | 周期（估） | 目标 | 依赖 | 对应需求 |
|---|---|---|---|---|
| **M1 播放内核 Demo** | 2 周 | 证明 `--wid` + gpu-next + d3d11va 在本机片源上"不绿、不发灰、字幕对、GPU 低" | IN-01~04、IN-08、T1 编译 | PLY-02~13、NFR-01/02 |
| **M2 Emby 接入** | 2 周 | 登录→浏览→播放→进度上报闭环（真服务器） | M1、IN-05、T3 | CON-01~04、LIB-01~04、SYN-01~03 |
| **M3 UI** | 3 周 | 完整壳层 + 详情 + 播放页 + 设置 + 无边框窗口 | M2、T4、IN-06 | LIB-01~04、WIN-01~04、SET-01~03 |
| **M4 打包** | 1 周 | 干净机器可安装、可启动、可播放的分发版本 | M3、IN-07 | PKG-01~03、NFR-04 |

### M1 播放内核 Demo（约 2 周）

范围：Qt 主窗口 + VideoHost 的 native 子窗口 + libmpv `--wid`；本地文件播放；mpv OSC；键位；S1~S4 测试；播放统计（I 键）；全屏；窗口缩放。

**退出标准（全部满足才算过）：**

1. Windows 10/11 上 MSVC 2022 x64 Release 编译通过；exe 启动无缺失 DLL；能打开本地视频文件。
2. 至少 2 个 GPU 厂商的适配器（同一台机器的独显+集显也可）上，`hwdec-current` 以 `d3d11va` 开头且不含 `-copy`；`current-vo`。
3. S1 HDR10 播放 10 分钟：不发灰、不偏色，与同机 mpv CLI 参考截图肉眼无可辨差异。
4. S2 DV P5、S3 DV P8 播放各 10 分钟：无绿屏/紫屏，颜色正常，无崩溃。
5. S4 ASS 特效字幕：位置/移动/渐变/淡入淡出/裁剪/卡拉 OK 正确，嵌入字体正常，10 分钟 `frame-drop-count` ≤ 10。
6. 4K HEVC 10bit 60Mbps 播放 10 分钟：GPU 3D 引擎占用 ≤ 同机 qEmby 同片基线的 50%；Copy 引擎 ≈ 0（<2%）；CPU 有可复现数值记录。
7. 控制条与键位：空格、←→、Shift+←→、↑↓、F、Esc、I、N/P 全部可用；OSC 播放/暂停、进度、音量、全屏按钮可用；窗口从 320×180 到 4K 缩放不黑屏、不崩溃。
8. 2 小时循环播放无崩溃/黑屏；退出后任务管理器无残留 emby-pc 进程。
9. 产出 `docs/test-reports/M1-<date>.md`：机器配置、片源清单、mpv/libmpv 版本、每条数值、截图、已知问题。

### M2 Emby 接入（约 2 周）

范围：EmbyClient（T3）接入真实服务器；登录/设备 ID；Views/Items/PlaybackInfo；直连 URL（token 走请求头）；Sessions 上报；续播；基础错误处理。UI 可先用简单列表/命令行壳，不要求最终界面。

**退出标准：**

1. Emby 4.8/4.9 真实服务器登录成功；错误密码、超时、自签证书错误有可区分提示，不崩溃。
2. 媒体库 Items 分页：1000 项列表无重复/漏项，总数与 TotalRecordCount 一致；海报 URL 可加载。
3. PlaybackInfo 每次请求带 DeviceProfile；测试库直连播放率 ≥ 80%；直连 URL 不含 token（抓包/日志检查）。
4. 从 Item 播放成功；开始/暂停/继续/拖动/停止 均能在服务端 Dashboard 看到；进度误差 ≤ 10 秒；停止后位置误差 ≤ 2 秒。
5. 续播：服务端记录 10 分钟的位置，打开后从 10:00±2 秒开始；>95% 时从头播放。
6. 快速切换 Item 100 次无崩溃；断网 30 秒后恢复或给出可读错误；token 失效能跳登录/提示。
7. 产出 `docs/test-reports/M2-<date>.md` + 服务端 Dashboard 截图 + 关键 API 请求/响应样本（脱敏）。

### M3 UI（约 3 周）

范围：应用壳（侧边栏：首页/电影/剧集/设置 + 顶部搜索框占位）；首页（继续观看/最新）；媒体库海报网格；详情页；播放页（OSC + VideoHost）；设置页；深色 QSS；原生 DWM 无边框窗口；高 DPI；窗口状态恢复。

**退出标准：**

1. 首页/电影/剧集/设置四个页面可导航；搜索框存在（P0 可为占位，P1 实现搜索）。
2. 1000 部影片海报网格：连续滚动帧率 ≥ 55fps（1080p、集显），无重复/漏项；缩略图异步加载，不阻塞 UI。
3. 详情页：标题/年份/时长/评分/简介/季集列表与 API 一致；播放按钮从第一集或续播点开始；返回列表恢复滚动位置。
4. 播放页：OSC 全部按钮可用；全屏进出正常；所有 P0 键位生效（搜索框等文本输入聚焦时不误触发）。
5. 无边框窗口：最大化、拖到屏幕上/下/左/右边缘、Win11 左右分屏，截图最外 1px 无可见缝隙；Snap Layouts 正常；双击标题栏最大化/还原。
6. 主窗口可缩至 800×480 可用，播放区域可缩至 320×180；不存在 900×600/960×540 的硬编码下限。
7. 设置页：服务器地址/账号/退出登录、默认字幕语言、自动续播开关、日志目录、关于/版本；重启后设置保持。
8. 冷启动到可交互 ≤ 3 秒；页面切换 ≤ 300ms；播放中 UI 线程无 > 200ms 阻塞；2 小时播放 + 浏览无崩溃，RSS < 600MB。
9. 产出 UI 走查清单（对照 mockups）+ 关键页面截图（首页/详情/播放/设置/最大化贴边特写）。

### M4 打包（约 1 周）

范围：CMake install、windeployqt、libmpv DLL 打包、VC 运行库、Inno Setup 安装器、版本信息、图标、卸载、崩溃日志、构建文档。

**退出标准：**

1. 干净的 Windows 10 22H2 与 Windows 11 23H2 x64（未装 Qt/VS）上：安装 < 2 分钟，桌面/开始菜单快捷方式可用，启动后能登录并播放 S5 基准片。
2. 安装包 ≤ 150MB；卸载后程序目录清理干净（用户数据默认保留，可选删除）；覆盖安装 0.1.0→0.1.1 保留设置与登录状态。
3. 不依赖开发机环境：无缺失 Qt 插件、VC++ 运行库、libmpv-2.dll。
4. 关于页显示应用/Qt/libmpv/构建时间；日志目录可打开；人为崩溃能生成 dump + 文本日志。
5. 发布物：安装包 + SHA-256 + 版本说明 + 构建文档（可复现步骤）；有代码签名证书则签名。
6. 冒烟测试：requirements 中全部 P0 条目在两台干净 VM 上通过（可复用 M1~M3 的测试报告 + 本轮复测记录）。

### D-5 关键路径与阶段门禁

- 关键路径：**IN-01 Windows 测试机 → M1 实机验证 → M2 真服务器联调 → M3 窗口/UI 验收 → M4 干净机安装**。
- T3（Emby API 客户端）和 T4（UI 骨架）可以在 Linux 上并行开发，但 **M2/M3 的最终验收必须在 Windows 实机完成**。
- 每个里程碑的门禁：lead 按 requirements.md 的 P0 条目逐项核对；不通过则列出 blocker 与修复日期；**不允许"先发布再修"跳过 P0 安全/崩溃类问题**。

---

## E. 需要 lead / 用户现在拍板的事项（按优先级）

1. **Windows 测试机**：谁提供、什么型号、何时可用？（最高优先级，决定 M1 能否开始）
2. **libmpv 分发许可**：GPLv2+ 还是 `-Dgpl=false` 的 LGPL 构建？（决定 IN-03 用哪个二进制、M4 能否闭源分发）
3. **测试素材**：S1~S4 是否能从现有片库提供？不能的话是否接受用公开样片/自制 ASS 测试文件？
4. **Emby 测试账号**：能否建一个临时非管理员账号，测试期间可用？（M2 必需）
5. **目标显示器分辨率/刷新率**：1080p60 还是 4K/高刷？（决定默认缩放与性能验收基准）
6. **项目名/品牌 + 窗口最终形态**：产品名/exe 名/图标；是否确认"无边框（原生 DWM）"作为最终形态？（不影响 M1，但影响 M3）

> 其余小决策（B 节）可按建议默认值直接执行；如无异议，lead 可在 team 里按默认值推进，不需要逐项回复。
