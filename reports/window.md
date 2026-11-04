# qEmby 窗口问题分析报告：最小尺寸下限与贴边缝隙

分析对象：`/root/github/emby-pc/qEmby`（commit f551964，只读）+ `libs/qwindowkit`（AlanHJ fork，commit 0e46ba6，声明版本 1.4.1.0；上游对照 1.4.0）。
分析时间：2026-11-04。仓库内文件均未修改，本报告为唯一写入文件。

## 0. 结论速览

| 问题 | 直接根因 | 关键证据 | 性质 |
|---|---|---|---|
| A 窗口不能缩小 | Home 模式硬编码 `setMinimumSize(QSize(900,600))`；Login 模式是 `setFixedSize()` 固定 450x320 | mainwindow.cpp:1065、1062-1063；layoututils.cpp:13-35 | 900x600 是硬编码的“设计下限”，Login 固定尺寸是设计如此 |
| A 的第二层下限 | `QWK::WindowBar` 的 QSS 给每个 system-button 设 `min-width: 50px`，Home 同时显示 7 个，加 380px 固定搜索框 | dark-style.qss:307-314、336-338；light-style.qss:313-320、342-344；mainwindow.cpp:295-297 | 设计（触控友好）但意外形成约 790-830px 的硬地板 |
| B 贴边缝隙（主） | `QWINDOWKIT_ENABLE_WINDOWS_SYSTEM_BORDERS=OFF` 后，QWindowKit 把客户区边缘当作 resize 热区；winIdChanged 设 `extra-margins=(1,1,1,1)`，即 `DwmExtendFrameIntoClientArea(1,1,1,1)`，Win11 DWM 再画 1px 外框 | CMakeLists.txt:99-106；win32windowcontext.cpp:909-916、1043-1046、925-927；QWindowKit CMakeLists.txt:48-52 | QWindowKit 在关闭系统边框模式下的已知代价（官方 README 自述“part of the client edge area will be occupied as the resizing margins”） |
| B 次因 | 最大化时 `WM_NCCALCSIZE` 按 `SM_CXSIZEFRAME + SM_CXPADDEDBORDER`（100% DPI 为 8px，150% 为 12px）内缩客户区；这是对 Windows 最大化窗口外扩的正常补偿，但 Win11/混合 DPI 下实际外扩量与 8px 不一致时会留 8px 级缝 | win32windowcontext.cpp:2191-2210；qwkwindowsextra_p.h:492-499 | 上游 1.4.0 同款逻辑，属“补偿量不等于实际值”的风险，不是必现 |
| B 次因 | 分数 DPI 下 Qt 逻辑像素/物理像素取整，叠加 1-2px | win32windowcontext.cpp:1667、2402；qwkwindowsextra_p.h:456-470 | 次要放大因素 |
| B 被 A 放大 | 900x600 的最小尺寸会让 Aero Snap/分屏区域小于 900px 时无法贴齐（如 1366 屏半屏 683px） | mainwindow.cpp:1065；QWindowKit 无 WM_GETMINMAXINFO 处理（全仓 grep 无匹配） | A 引起 B 的一种“大缝”场景 |

> 一句话：**A 的硬地板是 mainwindow.cpp:1065 的 900x600（第二层是 WindowBar 的 QSS min-width）；B 在 qEmby 里主要是 QWindowKit 关闭系统边框后的 1px DWM frame / resize-margin 机制，最大化分支还叠加 8px 级的补偿风险，分数 DPI 再叠加 1-2px。**

## 1. 问题 A：窗口大小有下限

### 1.1 applyWindowSizeMode 的三态

`MainWindow::applyWindowSizeMode(WindowSizeMode)`（mainwindow.cpp:1055-1075）只有三个分支：

| 模式 | 代码 | 行为 |
|---|---|---|
| Transition（过渡态） | mainwindow.cpp:1060-1061：`setMinimumSize(0,0)`、`setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX)` | 清空所有尺寸锁，窗口可自由缩放（仅用于登录/主页切换动画期间） |
| Login（登录页） | mainwindow.cpp:1062-1063：`LayoutUtils::fixWindowToContents(this, QSize(m_defaultWidth, m_defaultHeight))` | `m_defaultWidth=450`、`m_defaultHeight=320`（mainwindow.h:66-67）；layoututils.cpp:13-35 计算 `target = preferred.expandedTo(window->layout()->totalMinimumSize()).boundedTo(screen)` 后 `setFixedSize(target)` |
| Home（主界面） | mainwindow.cpp:1065：`setMinimumSize(QSize(900, 600).boundedTo(screen()->availableGeometry().size()))` | 硬编码 900x600 最小值；1067 随后同步原生 WS_MAXIMIZEBOX 能力 |

`applyWindowSizeMode(Home)` 不是在进入主页时立刻调用，而是在 `navigateToHome()` 的几何动画结束后才调用（mainwindow.cpp:1195），因此窗口会先以 Transition（无最小限制）从 450x320 动画放大到 1280x800，最后才被 900x600 锁住。

### 1.2 完整约束链（Top → Down）

```text
QWidget::minimumSize (mainwindow.cpp:1065, 900x600)
  → QWidgetPrivate::setConstraints_sys()
  → QWindow::setMinimumSize() / QWindowsWindow::setGeometry
  → WM_GETMINMAXINFO.ptMinTrackSize（Qt QPA 设置，用户拖动边框/系统 Snap 时生效）
```

Qt 文档明确：**显式设置的 `minimumSize` 会覆盖布局的 `minimumSizeHint`**（“If minimumSize() is set, the minimum size hint will be ignored”）。所以只要 mainwindow.cpp:1065 生效，窗口的硬地板就是 900x600，视图内部再大的 minimumSizeHint 都只能被裁切、不会把地板继续抬高。

但“布局约束”仍然影响另外两条路径：(a) Login 模式的 `fixWindowToContents()` 用的是 `QMainWindowLayout::totalMinimumSize()`；(b) Transition 模式 `minimumSize=0` 时，实际地板由布局 `minimumSizeHint()` 决定。完整层级如下表：

| 层级 | 对象 | 数值/行为 | 证据 | 是否卡住窗口 |
|---|---|---|---|---|
| L0 | MainWindow 显式 minimumSize | 900x600（Home）/ 0（Transition）/ 固定 450x320（Login） | mainwindow.cpp:1060、1065；layoututils.cpp:33 | **是（A 的直接原因）** |
| L1 | QMainWindowLayout::totalMinimumSize | menuWidget（QWK::WindowBar）+ centralWidget（m_viewStack）+ 边距 | mainwindow.cpp:388 `setMenuWidget(windowBar)`；layoututils.cpp:20-26 | Login 固定尺寸路径使用；Home 被 L0 覆盖 |
| L2 | m_viewStack（QStackedWidget） | QStackedLayout::minimumSize 取**所有页面的最大值**（不是当前页）；页面 = LoginView + HomeView | mainwindow.cpp:422-432 | Transition 模式下的地板 |
| L3 | HomeView | QVBoxLayout：m_contentLayout（QHBoxLayout：m_contentSwitcher + 可选 sidebar）+ m_miniMusicPlayer | homeview.cpp:151-257 | 经 L2 传播 |
| L4 | m_contentSwitcher（SlidingStackedWidget，继承 QStackedWidget） | 同样取所有页面最大值；动态页最多保留 12 个（`MAX_ACTIVE_VIEWS=12`），返回时 dispose | slidingstackedwidget.h:14-54；homeview.cpp:1156-1174、1276-1279 | 若页面有未截断的大 minimum 会传播 |
| L5a | DashboardView | 外层 QScrollArea（widgetResizable），minimumSizeHint 只有滚动条+边框 | dashboardview.cpp:219-222 | 否（被 QScrollArea 截断） |
| L5b | DetailView | 外层 QScrollArea；海报 250x375、左右 margin 40、底部卡片 minWidth 280 全在 scroll 内容里 | detailview.cpp:493-493、515、526；detailbottominfowidget.cpp:404 | 否（被 QScrollArea 截断） |
| L5c | LibraryView | 顶部 header 是固定高 65 的 QScrollArea；主体是 QListView | libraryview.cpp:44-54、71-72 | 否 |
| L5d | PlayerView | 无 QLayout（全部绝对定位 overlay）；右栏 fixedWidth 220 | playerview.cpp:2509 | 否（无布局，minimumSizeHint 约 0） |
| L5e | SettingsView | 左侧 `m_leftPanel->setFixedWidth(260)` + SlidingStackedWidget（每页 QScrollArea） | settingsview.cpp:40、98-123、133-137 | ~280-320px，低于 L0 |
| L5f | ManageView | 左侧 `m_leftPanel->setFixedWidth(260)` + 每页 QScrollArea | manageview.cpp:57、127-137 | ~280-320px，低于 L0 |
| L5g | HomeView 侧栏 | `kFloatingSidebarWidth=240`、`kPinnedSidebarWidth=136`，`setFixedWidth(sidebarWidthForMode())`；默认 floating，不在 layout 内（父级是 this，手工定位） | homeview.cpp:64-65、1789、624-631、2058-2063 | 默认不贡献；pinned 时 136 |
| L6 | QWK::WindowBar | QSS `min-width: 50px` × 7 个可见 system-button + fav margin 10 + 搜索框固定 380x32 + 标题最小宽度 | dark-style.qss:307-314、336-338；light-style.qss:313-320、342-344；mainwindow.cpp:295-297、368、370-381、461-473 | **是（第二层地板，约 790-830px）** |

### 1.3 视图里那么多 setMinimumWidth 为什么大多不是元凶

1. **QScrollArea 截断**：`QAbstractScrollArea::minimumSizeHint()` 只由滚动条和 frame 计算（约 20-40px），不会把 `widgetResizable` 的内容 minimum 传上去。DetailView（detailview.cpp:493）、SettingsView（settingsview.cpp:133-137）、ManageView（manageview.cpp:127-137）、DashboardView（dashboardview.cpp:219-222）都套了 QScrollArea，所以 `pageplayer.cpp:864 blockedKeywordsInput->setMinimumWidth(360)`、`settingscard.cpp:111 m_controlWidget->setMinimumWidth(220)`、`detailbottominfowidget.cpp:404 card->setMinimumWidth(280)` 这类值都被挡在 scroll 内。
2. **QStackedWidget 取所有页面最大值**：`QStackedLayout::minimumSize()` 是遍历所有 item 取 `expandedTo`，不是只看当前页。代码已经在用 `QSizePolicy::Ignored` 对抗它：`m_homeView->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored)` 在 mainwindow.cpp:785、1248，进入 Home 时又在 1116 恢复为 `Expanding/Expanding`。这说明作者知道“堆栈页会传播 minimum”，并刻意在 Login/Transition 期间切断。
3. **自定义 minimumSizeHint 已被用来“归零”宽度**：`horizontalwidgetgallery.h:31-32`、`horizontallistviewgallery.h:63-64`、`mediacarouselwidget.h:53-54` 都返回 `QSize(1, height)`；`horizontalwidgetgallery.cpp:21、29` 还有 `setMinimumWidth(1)`。这是针对布局 minimum 传播的既有规避，说明大面积 setMinimumWidth 并不是当前窗口下限的来源。
4. **PlayerView 不是元凶**：PlayerView 没有 QLayout，子控件全部是手动 `setGeometry` 的 overlay（setupUi 中 m_topHUD/m_bottomHUD/m_rightSidebar 等），QWidget 的 `minimumSizeHint()` 在无布局时基本为 0；只有独立播放窗口 `PlayerWindow` 在 playbackmanager.cpp:853 显式设了 960x540（那是独立窗口的设计值）。

### 1.4 第二层硬地板：QWK::WindowBar 的 QSS

`QWK::WindowBar` 自身 C++ 没有设置任何 minimum（全 QWindowKit src/examples grep `setMinimumSize/setMinimumWidth/minimumSizeHint` 无结果），最小宽度完全来自 qEmby 的 QSS：

```text
QWK--WindowBar QAbstractButton[system-button=true] { min-width: 50px; ... }   // dark-style.qss:307-314 / light-style.qss:313-320
QWK--WindowBar QAbstractButton#fav-button { margin-right: 10px; }             // dark-style.qss:336-338 / light-style.qss:342-344
m_globalSearchBox->setFixedSize(380, 32);                                     // mainwindow.cpp:297
```

Home 模式可见的 `system-button=true` 按钮：back、home、fav（mainwindow.cpp:461-473 显示）、theme（setPinButton 到 pin-button，mainwindow.cpp:269、175）、min、max、close（mainwindow.cpp:374-377），共 7 × 50 = 350px，加 fav margin 10px、加搜索框 380px、加标题最小宽度（约 45px，QSS min-height 32px 只影响高度），合计约 **785-830 逻辑像素**。

这解释了为什么“把 900 改成 0”不一定能缩到很小：下一个地板就是 WindowBar 的约 790px。用户如果只看到“缩到 900 左右停住”，是 L0；如果改成 0 后只能到约 790，则是这一层。

### 1.5 哪些是“有意设计”，哪些是“硬编码 bug”

| 项 | 判断 | 理由 |
|---|---|---|
| Login 固定 450x320（`fixWindowToContents` → `setFixedSize`） | 设计如此 | 登录页是独立的小窗，不允许缩放是产品决定；layoututils.cpp:30-34 同时把 `maximumSize` 也设成 target |
| `PlayerWindow->setMinimumSize(960, 540)`（playbackmanager.cpp:853） | 设计如此 | 独立播放窗口需要最小播放面积 |
| Home `setMinimumSize(900,600)`（mainwindow.cpp:1065） | **半设计半硬编码 bug** | 设计意图可理解（保证侧栏 240 + 内容可读），但用固定字面量；未按 DPI/屏幕尺寸/窗口所在屏幕计算；`boundedTo(screen()->availableGeometry().size())` 在 900x600 大于屏幕时会退化成“最小=屏幕”，窗口永远无法小于屏幕；也没有“紧凑模式”开关；且只在动画结束后才设置，过渡期间仍可能被用户拖小 |
| WindowBar QSS `min-width: 50px` | 设计（触控友好）但副作用大 | 7 个按钮叠加约 360px；没有随窗口变窄自动降级/隐藏按钮 |
| View 内大量 `setMinimumWidth/FixedSize` | 多数是局部布局设计，不是窗口下限根因 | 绝大多数被 QScrollArea 截断；少数在上表中已列 |

### 1.6 最小改法（改哪一行、改成什么）

**最小 patch（只改 qEmby，不动 QWindowKit）**：

1. `src/qEmbyApp/mainwindow.cpp:1065`：
   - 方案 A（推荐，真正可缩小）：
     ```cpp
     // 原来是 setMinimumSize(QSize(900, 600).boundedTo(screen()->availableGeometry().size()));
     setMinimumSize(QSize(640, 400).boundedTo(screen()->availableGeometry().size() - QSize(40, 40)));
     ```
   - 方案 B（完全交给布局/滚动区）：`setMinimumSize(0, 0);`，但必须同时做第 2 条，否则会被 WindowBar 约 790px 卡住。
2. 同文件 `mainwindow.cpp:295-297`：把 `m_globalSearchBox->setFixedSize(380, 32)` 改为可收缩（`setMinimumWidth(120); setMaximumWidth(380);` + 在窄窗口下 `hide()`），并把 `dark/light-style.qss:307-314/313-320` 的 `min-width: 50px` 降到 32-36px（或对 back/home/fav 在 width < 820 时隐藏，已有 `QWK::WindowBar` 的 `setVisible` 逻辑可复用）。
3. 保留所有内容页的 QScrollArea（现状已满足）；确保新增页面不要直接把大 minimum 的控件塞进非滚动 QHBoxLayout。
4. 如果只想改一行且接受 900 下限，至少改成：`setMinimumSize(QSize(900, 600).boundedTo(screen()->availableGeometry().size() - QSize(40, 40)));`，避免小屏幕上最小=屏幕。

**验证**：`mainwindow.cpp:1068-1074` 已经打印 `[MainWindow] Window size mode | ... | minimum: ... | nativeMinimum: ...`，运行日志 grep `[MainWindow] Window size mode` 即可看到 900x600；改后再看是否变小。

## 2. 问题 B：贴边（拖到屏幕边缘/最大化/分屏）有缝

### 2.1 三类机制与 qEmby 对应证据

| 类别 | 机制 | 缝宽 | 出现场景 | qEmby/QWindowKit 证据 | 判定 |
|---|---|---|---|---|---|
| (i) 自绘/无系统边框把客户区边缘当 resize 热区 | 关闭系统边框后，窗口把外侧 8px（`SM_CXSIZEFRAME+SM_CXPADDEDBORDER`）作为 hit-test 热区；再用 `DwmExtendFrameIntoClientArea(1,1,1,1)` 把 DWM 玻璃边伸进客户区 1px | 1px（两窗相接约 2px） | 普通拖动贴边、Aero Snap、分屏都会有 | qEmby CMakeLists.txt:99-106 明确 OFF；QWindowKit 自述 CMakeLists.txt:48-52 “part of the client edge area will be occupied as the resizing margins”；win32windowcontext.cpp:909-916 设 extra-margins=(1,1,1,1)；1043-1046 转成 DwmExtendFrameIntoClientArea；925-927 仍加 `WS_THICKFRAME|WS_CAPTION` 让 DWM 画 1px 外框 | **主因（最符合“一道缝/1-2px”）** |
| (ii) DWM 最大化边框补偿 | 最大化时窗口实际比 work area 大 `frameSize`，`WM_NCCALCSIZE` 把客户区四边内缩 `frameSize` 补偿；正常时刚好等于 work area，补偿量与实际外扩量不一致时就会露出 frameSize 的带 | 8px @100%、12px @150%（若异常） | **只在最大化**（`IsZoomed` 为真）时出现；普通 Aero Snap 不触发 | win32windowcontext.cpp:2191-2210：`max && !full` 时 `top += frameSize`，且 `!isSystemBorderEnabled()` 时 `bottom/left/right -= frameSize`；`frameSize = getResizeBorderThickness`（qwkwindowsextra_p.h:492-499 = SM_CXSIZEFRAME + SM_CXPADDEDBORDER）；上游 QWindowKit 1.4.0 同款（见第 4 节 diff 结论），本意是补偿而非制造缝隙 | **结构风险（若用户看到 8px/12px 宽边，就是它）** |
| (iii) 高 DPI 下 DPR 取整 | Qt 逻辑像素与 Win32 物理像素换算按 1/DPR 取整，窗口/客户区边界可能差 1px | 1-2px | 125%/150%/175% 等分数缩放时更明显 | win32windowcontext.cpp:1667 用 `QHighDpi::fromNativeLocalPosition`；2402 手乘 `devicePixelRatio()`；qwkwindowsextra_p.h:456-470 的 `getSystemMetricsForDpi` 在 API 不存在且 dpi!=96 时直接返回未缩放值 | **次要叠加项** |
| (iv) 最小尺寸把 Snap 区域卡住 | 窗口最小宽度 900 时，Windows Snap 到小于 900 的 zone 会被 `WM_GETMINMAXINFO.ptMinTrackSize` 钳到 900，贴不到屏幕边/另一个窗口 | 取决于屏幕，可能几十~几百 px | 分屏/贴边且 zone < 900 宽时（1366 屏半屏 683 < 900） | mainwindow.cpp:1065；QWindowKit 全仓没有 WM_GETMINMAXINFO 处理（grep 无匹配），故 Qt/QPA 默认把 QWindow minimum 作为 ptMinTrackSize | **A 引起 B 的场景** |

### 2.2 qEmby 具体属于哪一类

**主判定：(i) + (iii) 的 1px 缝；最大化时叠加 (ii) 的 8px 风险；分屏贴不齐时叠加 (iv)。**

- 为什么 (i) 是主因：qEmby 顶层 CMakeLists.txt:99-106 把 `QWINDOWKIT_ENABLE_WINDOWS_SYSTEM_BORDERS` 强制 OFF，注释自己写明“QWindowKit retains DWM shadows and handles resizing inside the client edges in this mode”。QWindowKit 的 CMake 选项说明也写明关闭后 “part of the client edge area will be occupied as the resizing margins”。这就是“自绘/无系统边框把客户区向内缩 N 像素作为 resize 热区”的教科书式描述。
- 1px 而不是 8px 的原因：普通（非最大化）窗口 `WM_NCCALCSIZE` 不修改 rgrc，客户区=窗口矩形；resize 热区只是 hit-test 语义（`win32windowcontext.cpp:1670-1678` 用 `frameSize` 判断鼠标是否在边框），不会缩小绘制区。真正可见的是 `DwmExtendFrameIntoClientArea(1,1,1,1)` 让 DWM 在客户区最外圈画 1px 边框（Win11 的 `DWMWA_VISIBLE_FRAME_BORDER_THICKNESS` 典型 1px，见 `qwkwindowsextra_p.h:474-490` 和 Microsoft 文档），以及窗口本身 `WS_THICKFRAME|WS_CAPTION`（`win32windowcontext.cpp:925-927`）后的 DWM 外框；两个贴在一起的窗口各 1px，视觉上约 2px。
- 为什么最大化可能是 8px：`win32windowcontext.cpp:2196-2210` 的 `frameSize` 在 100% DPI 是 `SM_CXSIZEFRAME(4)+SM_CXPADDEDBORDER(4)=8`，150% 是 12。上游注释解释这是为了把“最大化窗口比 work area 大一点”的部分裁掉；如果 Win11/24H2 的实际最大化外扩不是 8px（例如 DWM 只按 1px visible border 计算）或混合 DPI 下 `GetDpiForWindow` 与当前显示器不一致，就会真的留 8/12px 的黑边/背景带。
- 为什么 1-2px 可能是 DPI：Qt 6 默认 `PassThrough` 舍入策略，125% 下 1 逻辑 px=1.25 物理 px；窗口贴边时 `QWindow` 的 logical geometry 与 Win32 rect 的往返换算可能取整到 1px 的偏差，叠加 DWM 1px 后就是 1-2px。

### 2.3 Windows 11 Snap Layouts / Aero Snap / 普通拖动到边缘的区别

| 交互 | 触发方式 | 窗口状态（IsZoomed） | qEmby 路径 | 缝的主要来源 |
|---|---|---|---|---|
| 普通拖动到屏幕边缘 / Win+Left/Right（Aero Snap） | 拖动窗口到边缘或快捷键 | false（只是被 resize/position 到半屏/quarter） | `WM_NCCALCSIZE` 走非最大化分支（2191 之前），不执行 2196 的 inset | (i) 1px DWM frame + (iii) DPI 1px；若 zone < 900 宽则 (iv) 无法贴齐 |
| 最大化（标题栏双击 / 自定义 max 按钮 / Win+Up） | `showMaximized()`（mainwindow.cpp:646-655）或系统命令 | true（`GetWindowRect` 超出屏幕） | `WM_NCCALCSIZE` 执行 2196-2210：top += frameSize，且 OFF 时 bottom/left/right -= frameSize | (ii) 8/12px（若补偿量不匹配）+ (i) 1px |
| Windows 11 Snap Layouts（悬停最大化按钮弹出布局选择） | 最大化按钮 hover（Win11） | 选择某个 zone 后是 Snap，不是 IsZoomed | QWindowKit `WM_NCHITTEST` 对 Max 按钮返回 `HTZOOM`（win32windowcontext.cpp:1748-1749，HTZOOM==HTMAXBUTTON），所以布局浮层能弹出；选zone后同 Aero Snap | (i) 1px；可能叠加 (iv) 最小尺寸钳制；Win11 圆角也可能带来 1px 视觉差 |
| 全屏（F11/全屏按钮） | `showFullScreen()` | `isFullScreen()` 另判（`GetWindowRect` 等于 monitor rcMonitor，win32windowcontext.cpp:153-158） | `full` 为真，2196 不执行 | 无（全屏无边框） |

> 补充：Windows 11 的 Snap Layouts 只在悬停“最大化按钮”的矩形时才出现；QWindowKit 在 `WM_NCHITTEST` 里把 Max 按钮映射为 `HTZOOM`（1748-1749），并把非客户区鼠标消息转发给 Qt（`emulateClientAreaMessage`，1245-1336），因此 Snap Layouts 可用。但 Snap 之后的窗口不是 `IsZoomed`，不会触发 8px 的 max 分支，只是普通的 zone 尺寸，所以“Snap Layouts 有缝”和“最大化有缝”要分开测。

### 2.4 用户可自测的判据（PowerShell / WinSpy++ / 环境变量）

**判据 1：量 GetWindowRect vs 工作区 vs 客户区（最直接）**

```powershell
Add-Type @'
using System;
using System.Runtime.InteropServices;
public struct RECT { public int Left, Top, Right, Bottom; }
public struct MONITORINFO { public int cbSize; public RECT rcMonitor; public RECT rcWork; public int dwFlags; }
public class W {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern IntPtr MonitorFromWindow(IntPtr h, int flags);
  [DllImport("user32.dll")] public static extern bool GetMonitorInfo(IntPtr m, ref MONITORINFO mi);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref System.Drawing.Point p);
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out RECT r, int size);
}
'@
$h = (Get-Process qEmby).MainWindowHandle
$wr = New-Object RECT; [W]::GetWindowRect($h, [ref]$wr) | Out-Null
$cr = New-Object RECT; [W]::GetClientRect($h, [ref]$cr) | Out-Null
$mi = New-Object MONITORINFO; $mi.cbSize = [Runtime.InteropServices.Marshal]::SizeOf($mi)
[W]::GetMonitorInfo([W]::MonitorFromWindow($h, 2), [ref]$mi) | Out-Null
$ef = New-Object RECT; [W]::DwmGetWindowAttribute($h, 9, [ref]$ef, 16) | Out-Null  # 9 = DWMWA_EXTENDED_FRAME_BOUNDS
"window : $($wr.Left),$($wr.Top),$($wr.Right),$($wr.Bottom)  size=$($wr.Right-$wr.Left)x$($wr.Bottom-$wr.Top)"
"client : $($cr.Right)x$($cr.Bottom)"
"work   : $($mi.rcWork.Left),$($mi.rcWork.Top),$($mi.rcWork.Right),$($mi.rcWork.Bottom)  size=$($mi.rcWork.Right-$mi.rcWork.Left)x$($mi.rcWork.Bottom-$mi.rcWork.Top)"
"ext    : $($ef.Left),$($ef.Top),$($ef.Right),$($ef.Bottom)"
```

| 观察 | 结论 |
|---|---|
| 最大化时 windowRect = work area 四边各外扩 8px（例如 -8,-8,1928,1048，work=0,0,1920,1040），client = 1920x1040 | `WM_NCCALCSIZE` 补偿正确，8px 分支没有留缝；剩下的 1px 是 DWM frame |
| 最大化时 windowRect = work area（0,0,1920,1040），但 client = 1904x1024（四边各少 8） | 2196-2210 的 frameSize 补偿在“窗口没有外扩”的 Win11 上多减了 8px → 8px 缝，确诊 (ii) |
| windowRect 与 `DWMWA_EXTENDED_FRAME_BOUNDS` 相差 1px（左右/下） | 这是 DWM 的 visible frame border（Win11 典型 1px）→ (i)/(a) |
| 只有 125%/150% 缩放时出现 1-2px，`QT_SCALE_FACTOR_ROUNDING_POLICY=Round` 后变化 | DPI 取整 → (iii) |
| 半屏/分屏时窗口宽度明显大于半屏（如 1366 屏上 900px 而不是 683px） | 最小尺寸 900 钳制 → (iv)，先修 A |

**判据 2：一键对照实验**
- 设置环境变量 `QT_SCALE_FACTOR=1`（强制 100%）再贴边：若缝消失/变化 → (iii) DPI。
- 用 CMake 重新配置 `-DQWINDOWKIT_ENABLE_WINDOWS_SYSTEM_BORDERS=ON` 重新编译同一版本：若最大化/贴边的缝明显变化 → (i)/(ii) 与这个开关相关（注意 Win10 会恢复黑色顶边 workaround，见 2.5）。
- qEmby 已内置日志：mainwindow.cpp:661-669 在主题切换时打印 `systemBorders:`（`agent->windowAttribute("windows-system-border-enabled")` 的值）；正常应为 `false`，可确认走的是关闭系统边框分支。
- 用 WinSpy++/Spy++ 看 `GetWindowRect`、`GetClientRect`、`WS_STYLE`（应含 `WS_THICKFRAME|WS_CAPTION`，不含 `WS_SYSMENU`，见 win32windowcontext.cpp:925-927）。

### 2.5 修复建议（qEmby 最小 patch vs 自研客户端）

**qEmby 侧最小 patch（按侵入程度排序）**：

1. **先修 A 的最小尺寸**（mainwindow.cpp:1065），这会顺带解决“分屏贴不齐”的大缝（iv）。
2. **去掉 1px DWM 外框（Win11）**：在 `WindowUtils` 里拿到 HWND 后调用（Win11 22000+）：
   ```cpp
   constexpr DWORD kDwmwaBorderColor = 34;      // DWMWA_BORDER_COLOR
   constexpr COLORREF kDwmColorNone = 0xFFFFFFFE; // DWMWA_COLOR_NONE
   DwmSetWindowAttribute(hwnd, kDwmwaBorderColor, &kDwmColorNone, sizeof(kDwmColorNone));
   ```
   这能消掉 `DwmExtendFrameIntoClientArea(1,1,1,1)` 带来的 1px 边框；如果不要圆角，再加 `DWMWA_WINDOW_CORNER_PREFERENCE=DWMWCP_DONOTROUND(1)`。
3. **最大化 8px 风险（二选一）**：
   - 改 QWindowKit `libs/qwindowkit/src/core/contexts/win32windowcontext.cpp:2205-2209`：把 `frameSize` 的固定内缩改成“以 monitor work area 钳制”，即
     ```cpp
     if (max && !full) {
         MONITORINFO mi{ sizeof(mi) };
         ::GetMonitorInfoW(::MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &mi);
         clientRect->left   = qMax(clientRect->left,   mi.rcWork.left);
         clientRect->top    = qMax(clientRect->top,    mi.rcWork.top);
         clientRect->right  = qMin(clientRect->right,  mi.rcWork.right);
         clientRect->bottom = qMin(clientRect->bottom, mi.rcWork.bottom);
     }
     ```
     这样无论 Windows 实际把最大化窗口外扩多少，客户区都精确等于 work area（自动隐藏任务栏的 2px 分支仍保留在后面）。
   - 或者打开 `CMakeLists.txt:99-106` 的 `QWINDOWKIT_ENABLE_WINDOWS_SYSTEM_BORDERS=ON`，让系统边框走 DefWindowProc 分支（win32windowcontext.cpp:2163-2189），最大化时只补 top，左右下交给系统。代价：Win10 会启用 `Windows10BorderHandler` 的顶边 workaround（`windows10borderhandler_p.h:33-43`、`win32windowcontext.cpp:822-843` 画黑线），浅色主题下可能出现一条黑顶边；qEmby 已在 mainwindow.cpp:663-669 按主题设 `dark-mode`，可以配合使用。
4. **如果只想在 qEmby 层修而不动 QWindowKit**：可以给 MainWindow 安装 `nativeEvent` 过滤器，在 `WM_NCCALCSIZE`（wParam 为 TRUE）时自行把 `rgrc[0]` 钳到 work area，并让 QWindowKit 的处理不生效——但这会与 QWindowKit 的钩子竞争，不如直接改 QWindowKit 干净；不推荐作为首选。

## 3. 自研客户端的正确做法（Qt 6 原生 vs QWindowKit）

### 3.1 选型对比

| 维度 | Qt 6 原生（FramelessWindowHint + nativeEvent） | QWindowKit |
|---|---|---|
| 工作量 | 自己处理 WM_NCCALCSIZE / WM_GETMINMAXINFO / WM_NCHITTEST / WM_DPICHANGED / Snap Layouts | QWindowKit 已封装大部分；qEmby 这种用法只需 `WidgetWindowAgent::setup()` + setTitleBar/setSystemButton |
| 最大化/贴边精度 | 自己控制，能做到像素级 | 关闭系统边框时有 1px frame + 8px 补偿的已知代价；系统边框 ON 时 Win10 顶边有 workaround 绘制问题 |
| Snap Layouts | 需在 WM_NCHITTEST 返回 HTMAXBUTTON（HTZOOM）并转发 NCMOUSE 消息 | 已实现（win32windowcontext.cpp:1350-1560、1748-1749） |
| 多窗口/多显示器 | 自己管理每个 HWND | 1.4.1 fork 已改 per-window WndProc hash（见第 4 节），比上游 1.4.0 的单全局 WNDPROC 安全 |
| 建议 | 自研客户端建议直接用 Qt 6.5+ 的 `QWindow::startSystemResize()/startSystemMove()` + 少量 nativeEvent，代码量可控 | 若用 QWindowKit，**不要关闭系统边框**，或必须打 2.5 的补丁 |

### 3.2 必须做的四件事（Windows 10/11）

| 事项 | 正确做法 | 关键点 |
|---|---|---|
| `WM_NCCALCSIZE` | 返回 0（或 `WVR_REDRAW` 仅在无客户区时），客户区=窗口矩形；最大化时**只做“钳到 monitor work area”**，不要无条件内缩 `SM_CXSIZEFRAME+SM_CXPADDEDBORDER` | 先取 `MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST)` 的 `rcWork`；自动隐藏任务栏用 `SHAppBarMessage(ABM_GETSTATE)` 判断，任务栏所在边再缩 2px（QWindowKit 的 `kAutoHideTaskBarThickness` 就是 2，见 win32windowcontext.cpp:2217-2283） |
| `WM_GETMINMAXINFO` | `ptMaxPosition = rcWork.topLeft`、`ptMaxSize = rcWork.size`；`ptMinTrackSize` 用内容 minimum（不是 900 硬编码） | 这样最大化窗口矩形=work area，WM_NCCALCSIZE 就不需要做 8px 补偿；Aero Snap/分屏也按 work area 精确贴边。QWindowKit 1.4.1 **完全没有处理 WM_GETMINMAXINFO**（全仓 grep 无结果），这是它最大化/分屏不精确的根源之一 |
| `DwmExtendFrameIntoClientArea` | 需要 DWM 阴影时用 `MARGINS{1,1,1,1}`（或 0,0,0,1），并在 Win11 同时 `DwmSetWindowAttribute(DWMWA_BORDER_COLOR, DWMWA_COLOR_NONE)`；不需要阴影就传 0 | 1px margin 是 DWM 画外框/阴影用的，不是 resize 热区；不要把 resize margin 和 DWM frame 混为一谈 |
| `DWMWA_EXTENDED_FRAME_BOUNDS` | 取“用户看到的窗口边界”时用 `DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS=9, ...)`，不要用 `GetWindowRect` | `GetWindowRect` 含不可见的 resize border（Win10 典型 7-8px，左/右/下），两者差值就是“看不见的边框”；`DWMWA_VISIBLE_FRAME_BORDER_THICKNESS`（Win11，值随 DPI）给出 DWM 外框宽度。截图、窗口贴边对齐、窗口阴影计算都应基于 extended frame bounds |

### 3.3 推荐实现顺序（自研客户端）

1. `Qt::FramelessWindowHint` + `Qt::WindowMinMaxButtonsHint`；不要 `WA_TranslucentBackground`（会破坏 DWM frame/shadow，出现黑边）。
2. 实现 `nativeEvent`：`WM_NCCALCSIZE`（钳 work area）、`WM_GETMINMAXINFO`（ptMaxPosition/ptMaxSize=work area；ptMinTrackSize=内容 min）、`WM_NCHITTEST`（DPI 缩放的 6-8px 边条 + HTMAXBUTTON）、`WM_DPICHANGED`（用 `lParam` 的 suggested rect + `GetSystemMetricsForDpi` 重算 margin）。
3. 用 `QWindow::startSystemResize()/startSystemMove()` 处理拖动和边缘缩放，减少手写 hit-test 的 DPI 错误。
4. 若坚持用 QWindowKit：保持 `QWINDOWKIT_ENABLE_WINDOWS_SYSTEM_BORDERS=ON`（默认 ON），或使用本报告 2.5 的 `WM_NCCALCSIZE` work-area 钳制补丁 + `DWMWA_BORDER_COLOR=NONE`。
5. 窗口 minimumSize 只设 `QSize(0,0)`/内容级最小值，所有可能超宽的内容（设置页、详情页、播放器侧栏）都放 `QScrollArea`；`QStackedWidget` 的非当前页用 `QSizePolicy::Ignored`。

## 4. 附：QWindowKit fork 与上游 1.4.0 的关键差异（影响本问题）

用 `https://cdn.jsdelivr.net/gh/stdware/qwindowkit@1.4.0/...` 拉上游 1.4.0 与本地 fork 对比（fork 声明 1.4.1.0）：

| 差异 | 本地 fork | 上游 1.4.0 | 影响 |
|---|---|---|---|
| `setInternalWindowFrameMargins` | `win32windowcontext.cpp:99-102` 函数体开头直接 `return;`（后续 104-124 是死代码） | 上游 :94-115 正常调用 `setProperty("_q_windowsCustomMargins")` / `setCustomMargins()` | 让 Qt 知道自定义 frame margin；**只在 `isSystemBorderEnabled()` 为真时调用**（:648-651）。qEmby 的 OFF 构建不会走到，无直接影响；但如果按 2.5 方案打开系统边框，这里必须修掉，否则几何会错 1px 级 |
| 全局 WNDPROC | 改为 `g_originalWndProcs`（HWND→WNDPROC hash）+ per-window 安装/恢复（:600-645、:647-695） | 单个全局 `g_qtWindowProc` | fork 更好，支持多窗口/多 QWindow；上游单全局在多窗口下会串 |
| `DrawWindows10BorderHook_Native` | 画 1px 黑线（:822-843） | `int(getWindowFrameBorderThickness(hwnd))` | 只在系统边框 ON + Win10 生效 |
| 注释 | 大量注释被清空（不影响逻辑） | 有详细解释 | 本报告的机制解释引用了上游注释（如“最大化窗口实际比 work area 大一点”） |

结论：qEmby 的 B 问题不是 fork 引入的，fork 的 `nonClientCalcSizeHandler` 与上游 1.4.0 一致；fork 的 per-window WndProc 是正向修复；如果打开系统边框，需要先把 `setInternalWindowFrameMargins` 的 early-return 去掉。

## 5. 证据索引（file:line）

| 主题 | 位置 |
|---|---|
| 无边框窗口 agent | mainwindow.cpp:149-153；standaloneloginwindow.cpp:27-28；playerwindow.cpp:22-23 |
| WindowBar / 系统按钮 | mainwindow.cpp:264-288、368-381、461-473 |
| WindowBar QSS min-width | dark-style.qss:307-314、336-338；light-style.qss:313-320、342-344 |
| Home/Login 尺寸逻辑 | mainwindow.cpp:1055-1075、1104-1234、1236-1346 |
| 默认登录尺寸 | mainwindow.h:66-67 |
| fixWindowToContents | layoututils.cpp:13-35 |
| 视图最小约束（被 ScrollArea 截断） | detailview.cpp:493、515、526；settingsview.cpp:40、133-137；manageview.cpp:57、127-137；dashboardview.cpp:219-222；libraryview.cpp:44-54 |
| 自定义 minimumSizeHint=1 | horizontalwidgetgallery.h:31-32；horizontallistviewgallery.h:63-64；mediacarouselwidget.h:53-54；horizontalwidgetgallery.cpp:21、29 |
| QWK 系统边框开关 | CMakeLists.txt:99-106（qEmby）；libs/qwindowkit/CMakeLists.txt:35、48-52 |
| QWK 边框/边距 | win32windowcontext.cpp:83-96、909-927、1043-1046、2163-2189、2191-2210 |
| QWK 边框厚度 | qwkwindowsextra_p.h:474-499 |
| QWK hit-test / Snap | win32windowcontext.cpp:1670-1678、1748-1749；snapLayoutHandler :1350-1560 |
| QWK 10px 边框 workaround | windows10borderhandler_p.h:33-43；widgetwindowagent_win.cpp:19-177；widgetwindowagent.cpp:56-58 |
| QWK fork 差异 | win32windowcontext.cpp:99-102（early return）；对照 /tmp/qwk140.cpp:94-115（本次分析临时下载，不在仓库内） |

---

**说明**：以上所有 qEmby/QWindowKit 行号基于当前工作区 commit；`qEmby` 仓库未做任何修改。若需要我给出一份可直接 `git apply` 的 patch（mainwindow.cpp:1065 + QSS 两处 + QWindowKit win32windowcontext.cpp:2205-2209），可以在下一步生成。