# qEmby UI 参考与 token 映射（T43）

> 参考仓库：[/tmp/qEmby-ref](https://github.com/AlanHJ/qEmby)（MIT，只读参考；本文件用于记录设计来源与实际适配）
> 当前项目：Emby PC Qt6 Widgets，深色主题；播放内核（--wid + gpu-next）、DWM 无边框和输入转发保持不变。
> 原则：**高保真参考结构和视觉语言，不做像素级复制**；qEmby 的 QSS 颜色/间距/字号在本项目映射为 `app/src/ui/tokens.h` + `dark.qss`。

## 1. qEmby 源码位置

| 内容 | qEmby 文件/位置 |
|---|---|
| 深色主题/QSS 色板 | `src/qEmbyApp/resources/qss/dark-style.qss:1-8`（Slate 色板注释） |
| 全局滚动条 | `dark-style.qss:31-56` |
| 全局搜索框 | `dark-style.qss:110-149`；`src/qEmbyApp/mainwindow.cpp:291-303`（固定 380×32） |
| 悬浮侧边栏 | `dark-style.qss:1626-1760`；`src/qEmbyApp/views/user/homeview.cpp:64-65`（floating 240 / pinned 136） |
| 侧边栏导航项 | `dark-style.qss:1685-1735`（14px、padding 8/12、radius 8、selected #1E3A8A/#60A5FA） |
| 媒体库海报卡 | `dark-style.qss:1770-1795`（media-card radius 8；title 13/year 12） |
| 首页 section 标题/查看全部 | `dark-style.qss:1820-1858`（section title 20、see-all 14 #60A5FA） |
| 详情页标题/meta/chips/简介 | `dark-style.qss:2010-2105`（标题 28、section title 20、overview 14、chip 12） |
| 设置卡片 | `dark-style.qss:3284-3325`（#SettingsCard radius 12、title 14、desc 12） |
| 登录输入框 | `dark-style.qss:486-530, 637-700, 776-831` |
| 截图 | `screenshots/2.png` 首页；`3.png` 设置；`4.png` 管理；`5.png` 详情 |

## 2. qEmby token 表

### 2.1 颜色

| 语义 | qEmby 值 | qEmby 位置 | 本项目映射（tokens.h） |
|---|---|---|---|
| 主背景 | `#0F172A`（Slate-900） | dark-style.qss:23, 1626 | `ColorBg` |
| 侧边栏/卡片/输入 | `#1E293B`（Slate-800） | dark-style.qss:1632, 1770, 3288 | `ColorSurface` |
| 边框/悬浮 | `#334155`（Slate-700） | dark-style.qss:1634, 1778 | `ColorBorder` / `ColorSurfaceHover` |
| 按压/强边框 | `#475569`（Slate-600） | 多处 hover/pressed | `ColorSurfacePressed` / `ColorBorderStrong` |
| 主标题 | `#F8FAFC`（Slate-50） | dark-style.qss:75, 1832 | `ColorTextPrimary` |
| 正文 | `#CBD5E1`（Slate-300） | dark-style.qss:25, 1664 | `ColorTextBody` |
| 次要文字 | `#94A3B8`（Slate-400） | dark-style.qss:7, 1790 | `ColorTextSecondary` |
| 禁用/占位 | `#64748B`（Slate-500） | dark-style.qss:7, 3299 | `ColorTextDisabled`（只用于禁用/装饰） |
| 主蓝 | `#3B82F6`（Blue-500） | dark-style.qss:2 | `ColorAccent` |
| 主蓝 hover | `#2563EB`（Blue-600） | dark-style.qss:2 | `ColorAccentHover` |
| 主蓝 pressed | `#1D4ED8`（Blue-700） | dark-style.qss:3 | `ColorAccentPressed` |
| 链接蓝 | `#60A5FA`（Blue-400） | dark-style.qss:1827 | `ColorAccentLight` / `ColorFocus` |
| 成功 | `#22C55E` | MediaCardThemeHelper 进度 | `ColorSuccess` |
| 危险 | `#EF4444` / `#DC2626` | dark-style.qss:4 | `ColorDanger` / `ColorDangerHover` |

> 与旧主题差异：旧值 bg `#0B1220`、card `#111C2E`、border `#263449`、次要文字 `#64748B`；T43 统一切到 qEmby Slate 色板，并把信息性次要文字提升为 `#94A3B8`（WCAG AA 通过）。

### 2.2 间距阶梯

qEmby 实际使用以 4/8/12/16/20/24/32 为主（如侧边栏 padding 8/12、卡片 gap 16、section 间隔 24–32、页面 gutter 20–32）。建议映射：

| Token | 值 | 用途 |
|---|---|---|
| `Space1` | 4 | icon/text、chip 内 |
| `Space2` | 8 | 卡片内、按钮纵向 |
| `Space3` | 12 | 卡片 padding、行内 |
| `Space4` | 16 | 卡片 gap |
| `Space5` | 24 | section gap |
| `Space6` | 32 | 页面 gutter、topbar 左右 |
| `Space7` | 48 | 大区块 |

落地映射：页面左右 32；section 间距 24；卡片 gap 16；卡片内 12；侧边栏 nav padding 8/12。

### 2.3 字号/字重

| 语义 | qEmby 值 | qEmby 位置 | 本项目 token |
|---|---|---|---|
| 详情标题 | 28 / 800 | dark-style.qss:2010 | `FontDisplay` + 800 |
| 页面标题 | 24 / 700 | dark-style.qss:1798（library） | `FontH1` + 700 |
| 首页 section | 20 / 700 | dark-style.qss:1822 | `FontH2` + 700 |
| 详情 section | 20 / 800 | dark-style.qss:2058 | `FontH2` + 700/800 |
| 正文 | 14 | dark-style.qss:2044 | `FontBody` |
| 卡片标题 | 13 / 600 | dark-style.qss:1783 | `FontBodySmall` |
| meta/year | 12 | dark-style.qss:1790 | `FontCaption` |
| 设置项 title | 14 / 600 | dark-style.qss:3308 | `FontBody` + 600 |
| 设置项 desc | 12 | dark-style.qss:3316 | `FontCaption` |
| 最小信息字号 | 11–12 | qEmby 多处 | `FontMicro` 仅 badge/版本 |

### 2.4 圆角/尺寸

| 元素 | qEmby 值 | 映射 |
|---|---|---|
| 卡片 media-card | 8 | `RadiusMd` |
| 设置卡片 | 12 | `RadiusXl` |
| 主按钮/次按钮 | 8 | `RadiusMd` |
| 搜索框 | 16（胶囊） | 16 / `RadiusPill` 视觉 |
| chip/badge | 10–14 | `RadiusSm`/`RadiusXl` |
| 侧边栏 nav | 8 | `RadiusMd` |
| 图标尺寸 | 16–24（sidebar action 19/22，utility 11/13） | `IconSize` 18 / large 24 |
| qEmby 悬浮侧栏 | 240（pinned 136） | 本项目默认 **240**，保留 180–320 可拖拽（T38 功能） |
| 顶栏搜索 | 380×32 | 本项目 380×34（Windows 缩放留 2px） |

## 3. 与现有项目的差异清单

| 项目 | 旧值（T42 前） | T43 目标 | 处理 |
|---|---|---|---|
| 色板背景 | `#0B1220` | `#0F172A` | 全局 QSS 改 |
| 卡片/输入 | `#111C2E` | `#1E293B` | 全局 QSS 改 |
| 边框 | `#263449` | `#334155` | 全局 QSS 改 |
| 次要文字 | `#64748B`（3.59–3.93:1） | `#94A3B8`（6.66–7.3:1） | QSS 改；`#64748B` 仅 disabled |
| 侧边栏默认宽 | 224 | 240（qEmby floating） | `tokens::SidebarDefaultWidth`；保留 180–320 |
| 顶栏高 | 64 | 56 | `tokens::TopBarHeight` |
| 搜索框 | 左对齐 260–460×38 | 居中 380×34 胶囊 | `mainwindow.cpp` |
| 首页 section | 18px | 20px | QSS 改 |
| 详情标题 | 30px | 28px | QSS |
| 页标题 | 22/24混用 | 24 | QSS/token |
| 按钮圆角 | 7 | 8 | QSS |
| 焦点 | 全局 `outline:none` | 2px `#60A5FA` focus ring | QSS T43 规则 |
| 滚动条 | 10px #334155 实色 | 10px rgba(148,163,184,.30)，hover .65 | QSS |
| 字体族 | Qt 默认 | 保留系统 UI 字体（Segoe UI / Microsoft YaHei / Noto Sans CJK） | 不硬编码单一字体 |

## 4. 直接照搬 / 适配 / 不照搬

**直接照搬（颜色/尺度语言）**
- Slate 色板：`#0F172A` / `#1E293B` / `#334155` / `#475569`。
- 文本层级：`#F8FAFC` / `#CBD5E1` / `#94A3B8`。
- 主蓝 #3B82F6 / #2563EB；链接 #60A5FA。
- 滚动条 10px 半透明 handle、hover 加宽。
- 设置卡片 radius 12、媒体卡片 radius 8、按钮 radius 8。

**适配（保留本项目 DWM/布局/功能）**
- qEmby 使用 QWindowKit 自定义标题栏；本项目保留现有 WindowFrame/TitleBar（36px），顶栏作为 content topbar 设 56px，不复制 qEmby 的窗口栏结构。
- qEmby 侧边栏 floating 240 / pinned 136；本项目保留 180–320 可拖拽（T38），默认 240，不强行压到 136。
- qEmby 首页无侧边栏；本项目保留侧边栏，仅对齐 section 结构（标题 + “查看全部 >” + 海报）。
- qEmby 设置页左侧 260px 分类导航；T43 先做 token/分组卡片，T45 再评估是否改双栏。
- qEmby QSS 也有全局 `outline:none`；本项目**不照搬**，按 T42 P0 加焦点环。

**不照搬**
- qEmby 的 QWindowKit/QML/弹幕/特定组件类名；不引入新依赖。
- qEmby 的浅色/主题切换和品牌图标；只使用 MIT SVG 中必要的线性图标并注明来源。
- 不复制 qEmby 的布局代码/视觉资产到 T43 以外的功能实现。

## 5. 图标

- 本项目现有 `app/src/ui/icons.cpp` 为自绘线性矢量图标（Home/Movie/Tv/Search/Play/Pause/Fullscreen...），保持使用。
- 需要直接使用 qEmby 线性 SVG 时，文件放在 `app/src/ui/icons/qemby/`，并在 `app/src/ui/icons/ATTRIBUTION.md` 注明：
  - 来源 https://github.com/AlanHJ/qEmby
  - MIT License
  - 复制文件列表
- T43 先复制参考性的 6 个 player/search/settings SVG；实际接入在 T44/T45 按组件替换，不改变播放器内核。

## 6. 验收映射

- `tokens.h` 是代码侧唯一常量来源；`dark.qss` 的数值必须与本文 token 表一致。
- T43 selftest 断言：
  - tokens 常量存在（背景/文字/间距/圆角/侧边栏默认宽）。
  - dark.qss 加载成功，含 `:focus`、滚动条、`#0F172A` / `#1E293B` token 值。
  - 主窗口 topbar 高 56、sidebar 默认 240（QSettings 未设置时）。
- 截图：T43 更新首页/媒体库/详情/设置/登录；T44/T45 再针对页面结构细化。
