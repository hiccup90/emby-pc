# 决策清单 v4（HDR/DV 片源正确解码，输出 SDR；Qt 控制条）

> 需求已确认：
> * 杜比视界：**不要求原生点亮**，只要**不绿屏、颜色正确**；
> * HDR：**不要求原生 HDR 输出**，只要**不发灰**（正确 tone-map 到 SDR）；
> * 特效字幕：ASS 特效字幕要能正常显示。

---

## 0. 结论：架构不变，配置简化

| 约束 | 结论 |
|---|---|
| DV 正确解码（不绿）需要 **`vo=gpu-next`**（libplacebo） | 必须 |
| libmpv render API **不支持 gpu-next**（mpv #10810，2026-10 仍 OPEN） | render API 路线排除 |
| 所以视频层仍是 **`--wid` 子窗口 + `vo=gpu-next`** | 不变 |
| 不再需要原生 HDR | **不需要** `target-colorspace-hint`、不需要 D3D11 HDR swapchain |
| 输出 SDR => **Qt 覆盖窗口没有色差风险** | 播放控制条方案可以自由选 |

---

## 1. mpv 配置基线（HDR/DV → SDR，不发灰）

```ini
vo=gpu-next
gpu-api=d3d11
gpu-context=d3d11
hwdec=d3d11va              # 零回拷硬解
d3d11va-zero-copy=no       # 按机型灰度，默认关

# —— 输出 SDR ——
target-trc=srgb
target-prim=bt.709
target-colorspace-hint=no  # 不动 swapchain，避免 Windows HDR 模式下“发灰”
tone-mapping=auto          # gpu-next 下自动 = spline
hdr-compute-peak=auto      # 支持时逐场景分析峰值，避免死板
dither-depth=auto

# —— 字幕 ——
blend-subtitles=video      # 字幕参与视频色彩处理，避免 HDR 下字幕过亮/过暗
sub-ass-override=no        # 保留 ASS 原始样式与特效
embeddedfonts=yes
sub-auto=fuzzy             # 自动加载同名外挂字幕

# —— 播放器窗口行为 ——
osc=yes                    # 或换成自绘 Lua OSC
input-default-bindings=yes
```

**“发灰”最常见的三个原因**（都已被上面的配置规避）：
1. Windows 开着 HDR 模式、播放器按 SDR 输出 → SDR 内容被系统二次映射，看起来发灰。**建议 Windows HDR 关掉**，让 mpv 自己 tone-map。
2. 没有 tone mapping（把 PQ 当 SDR 直接输出）→ 灰蒙蒙、对比度低。
3. `target-trc/target-prim` 没显式设置，某些驱动/版本会猜错。

---

## 2. 播放控制条怎么自定义（回答"Lua 自绘能自定义吗"）

**能，而且比想象的多。** mpv 的 OSC 本身就是一份 Lua 脚本（`player/lua/osc.lua`，3500 行），有三档做法：

| 档位 | 做法 | 能改什么 | 工作量 |
|---|---|---|---|
| 1 | `--script-opts=osc-*` / `osc.conf` | 颜色（背景/标题/按钮/进度）、布局（bottombar / slimbox / box / floating）、进度条样式（bar/diamond/knob）、图标风格（layout/classic/fluent）、显隐时机、缩放、对齐、窗口按钮、缩略图尺寸 | 极低 |
| 2 | 复制一份 `osc.lua` 改代码 | 布局结构、按钮集合、交互逻辑、自定义菜单 | 中 |
| 3 | 完全自己写 Lua 脚本（`osc=no` + `script=你的.lua`） | 全部外观与交互 | 中高 |

**能做的**（基于 ASS/OSD 绘制）：
- 进度条、时间、标题、按钮、图标（ASS 矢量图形 / 字体图标）；
- 淡入淡出（`\fad`）、位移/缩放/旋转动画（`\t`）、裁剪（`\clip`）；
- 鼠标悬停/点击/滚轮（`observe_property("mouse-pos")` + 按键绑定）；
- 进度条悬停缩略图（配合 `thumbfast` 脚本，用 ffmpeg 生成）。

**不能做的**：
- 富文本排版、滚动列表、下拉菜单（可以用 ASS 模拟，但麻烦）；
- Web 级别的缓动动画、复杂过渡；
- 原生控件（输入框、复选框）——要输入文本只能用 mpv 的 `script-message` 或外部窗口。

> 你可以先看看 mpv 自带的 OSC 长什么样（`--script-opts=osc-layout=floating,osc-icon_style=fluent`），它已经挺现代；是不是满足你的设计需求，看一轮再说。

### 播放控制条的两个选择（现在都可行）

| 方案 | 优点 | 缺点 |
|---|---|---|
| **A. mpv OSC（内置/改/自写）** | HDR/色彩永远正确（OSD 在 mpv 内部画）；键鼠原生；工作量小 | 只能 ASS 绘制，复杂 UI 受限 |
| **B. Qt 透明覆盖窗口** | Qt 控件随便画，设计完全自由 | 要处理 z 序/焦点/全屏/DPI；**但现在输出 SDR，无 HDR 色差问题** |

建议：**先用 A 把播放跑通**（改 `script-opts` 或 fork osc.lua），如果设计稿确实需要 B，再补。

---

## 3. 已确认 / 待确认

**已确认（2026-11-04）：**
- 输出走 SDR，不做原生 HDR 输出；要求只是"不发灰、颜色对"。
- **不做 HDR 检测和提示**，默认假设 SDR。
- **DV P7 片源不多** → 用 mpv 0.41 稳定版即可（P5/P8 已够）；0.42 dev 暂不需要。

**待确认：**
1. ~~播放控制条 A（mpv OSC）还是 B（Qt 覆盖窗口）~~ → **已决策（2026-11-06）：B，保留 Qt 控制条**，见 §5。
2. 目标显示器分辨率/刷新率（决定默认缩放；24p 片源在 60Hz 屏上用 `video-sync=audio` 即可）。

---

## 4. 下一步

确认上面 1~3 后，我搭 Demo：
- Qt Widgets 窗口 + `QWidget::winId()` 作为 mpv `--wid` 宿主；
- 本地文件播放 + 硬解 + ASS 字幕 + OSC；
- 用 HDR10 / DV P5 / DV P8 / ASS 特效字幕四个样本验收"不绿、不发灰、字幕正常"。

---

## 5. 控制条方案决策（2026-11-06，v4）

**决策：保留 Qt 控制条（方案 B），不启用 mpv OSC（方案 A）。**

理由：
1. 当前配置输出 SDR（target-trc=srgb、target-prim=bt.709），Qt 覆盖窗口不会出现 HDR 色差风险；
2. Qt 控制条已实现并经 T45（分段 Seek/布局）、T51（命中/拖动后恢复）、T53（纯图标+tooltip）、T55（不透明命中区）修复，播放/暂停、上下集、进度、音量、字幕/音轨菜单、统计、全屏均有实现；
3. 不依赖 libmpv 发行包是否带 osc.lua，跨版本/跨发行包更稳；mpv OSC 仅保留 EMBY_PC_MPV_OSC=1 调试开关。

落地：
- docs/windows-acceptance.md 的 O-01~O-08 已改为 Qt 控制条验收（O-05 音量滑杆为 P1）；
- reports/playback-integration.md 中“隐藏 Qt 控制条、由 OSC 提供控制”的过时描述以后续修订为准；
- docs/requirements.md PLY-07 的“mpv OSC”文字由 planner 在下一版需求同步（本批不改需求文档）。
