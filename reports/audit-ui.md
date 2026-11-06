# UI/UX 全面审查 #2（T59）

- 审查范围：首页、媒体库、详情、播放器控制条、设置、登录、chrome/标题栏、空态/错误态、DPI/缩放、焦点/可访问性、文案残留
- 审查方式：只阅读代码与 preview，不修改代码；额外用 offscreen 探针检查 800×480 / 960×600 的 HomeView 实际 viewport/content 宽度，并用 `QT_SCALE_FACTOR=1.5` 重跑截图检查 DPI。
- 基线：commit `a28e44b`（T54–T57 后），`emby-ui-demo --selftest` 当前 190 PASS。
- **P0 数量：2**

## P0（阻塞级，建议优先修）

### P0-1 首页 section 内容横向溢出，960/800 宽度右侧被裁且无法横向滚动

- 文件：`app/src/ui/homeview.cpp:309-312`、`:667-671`、`:853-868`
- 现象：`HomeSectionsScroll` 设置 `widgetResizable(true)` + `horizontalScrollBarPolicy=AlwaysOff`，但 `resizeHomeSectionRows()` 对 resume content `setFixedSize(hint)` 后又调用 `m_sectionsContent->adjustSize()`。外层 content 被撑到约 1022px，大于 viewport。
- 实测（offscreen 800×480）：viewport=546、HomeSectionsContent width=1022、horizontalScrollBar maximum=476，而横向滚动条被关闭；`preview/06-home-960x600.png` 右侧继续观看第三张卡已部分裁掉。
- 触发：1280→960/800 窗口缩放，或从播放/全屏返回主页后恢复 800×480；正是用户反馈的「主页缩放/滚动异常」。
- 修复建议：`resizeHomeSectionRows()` 只设置各 section 高度，不对 `m_sectionsContent` 调 `adjustSize()` 撑宽；让 QScrollArea 的 widget 宽度始终等于 viewport，resume 横向滚动只作用于内层 `HomeSectionScroll`；`layoutPreviewRow()` 最小列数从 6 降到 2–4，并按 available 动态算卡宽；补 800/960 的 viewportWidth/contentWidth/horizontalMaximum 断言。
- 预估：0.5 天。

### P0-2 播放模式允许 320×180，但控制条在该尺寸下溢出/覆盖整个画面

- 文件：`app/src/ui/mainwindow.cpp:1332-1336`（playerMode 最小 320×180）、`app/src/ui/playerpage.cpp:292-314`（控制条 24px margins/6px spacing）、`:320`（播放按钮 48×48）、`:349`（字幕/音轨/统计 36×36）、`:362`（音量 96px）、`:797-805`（topHeight 76 + controlHeight 108）
- 现象：控制条固定按钮总宽约 394px，加左右 margins 48 + spacing 后超过 320；顶部 76 + 底部 108 = 184px，已大于 180px 窗口高度。进入播放模式后缩到允许的最小尺寸，控制按钮会重叠/被裁，顶部/底部浮层覆盖全部视频。
- 触发：播放模式 → 拖到 320×180（T52 明确允许的 minimum）；或恢复最小尺寸后再次进入。
- 修复建议：二选一——将播放模式 minimum 提高到 640×360；或在 <640 宽启用 compact 布局（缩小 margins、隐藏音量滑杆/次要按钮到 menu、控制条高度降到 72、单行 topBar）。补 320×180 的布局快照/按钮 rect 断言。
- 预估：0.75–1 天。

## P1（高优先级，影响关键操作/可访问性）

### P1-1 首页真实数据失败/加载中时 section 页面空白，无错误提示或重试

- 文件：`app/src/ui/homeview.cpp:638-645`（section.error 的 row 直接隐藏）、`:871-897`（`setError()` 只更新 legacy recent 与 resume 空态）、`app/src/ui/mainwindow.cpp:956-959`（错误回调调用 `m_home->setError()`）
- 触发：登录后网络错误、401、服务器离线，`fetchHomeSections` 返回 error 且 sections 为空。
- 现象：sectioned HomeView 仍是空内容区，用户看不到「加载失败/重试」；resume legacy 的 `m_resumeEmpty` 不在当前 section 页可见。
- 修复建议：HomeView 增加 section 级错误/加载占位（顶部 banner 或居中 EmptyState + 重试按钮），`setError()` 同时切到 section 页错误状态；`setLoading()` 同样在 section 页显示 skeleton/「正在加载首页…」。
- 预估：0.5 天。

### P1-2 首页卡片仅支持鼠标，键盘用户无法打开媒体

- 文件：`app/src/ui/homeview.cpp:27-140`（ResumeCard）、`:122-232`（HomeSectionCard）；均只有 `mousePressEvent`，没有 `setFocusPolicy`/keyPress，只有 `HomeSectionSeeAll` 有 StrongFocus（`:599`）
- 触发：Tab 键盘导航；焦点在「查看全部」时可操作，但卡片本身永远不会获得焦点。
- 修复建议：两个卡片控件改为 QAbstractButton 或设置 StrongFocus，实现 Enter/Space 触发 onClick；加 focus QSS 外框和 accessibleName（标题）。
- 预估：0.5 天。

### P1-3 设置页在 800×480 下媒体库行控件横向溢出

- 文件：`app/src/ui/settingspage.cpp:110`（左 nav 固定 232）、`:145`（右 content 左右 32 margin）、`:638-668`（名称最小 120 + 两个 checkbox + 上下移 28×2 + spacing）
- 触发：主窗口最小 800×480 → 设置 → 媒体库；主 sidebar 240 + settings nav 232 后内容区约 328px，扣 64 margin 后约 264px，而一行固定控件约 120+90+90+56+spacing ≈ 400px。
- 影响：媒体库显示/首页显示/排序按钮右侧被裁，设置功能不可用。
- 修复建议：800 宽下让 library row 换行（名称一行、checkbox 一行、排序按钮右侧），或设置页导航在窄窗口收缩为 180；给 SettingsLibraryRow 加换行布局和 800×480 断言。
- 预估：0.5 天。

### P1-4 播放器纯图标控件缺少可访问名称，overlay 显式 NoFocus

- 文件：`app/src/ui/playerpage.cpp:65-67`（makeIconButton 只有 tooltip，没有 accessibleName）、`:245`（顶层 overlay Qt::NoFocus）、`:331-347`（字幕/音轨/统计文本为空，只有 tooltip，T53）
- 触发：屏幕阅读器/纯键盘操作；图标按钮无可读名字，顶层浮层不接受键盘焦点。
- 修复建议：所有图标按钮设置 setAccessibleName(tooltip)，字幕/音轨 tooltip 作为 accessibleName；提供键盘快捷键（Space 播放、方向键 seek、Esc 返回）并在 overlay 上保留有限焦点策略。
- 预估：0.5 天。

### P1-5 登录对话框在低分辨率下固定 620 宽度、无密码显示切换

- 文件：`app/src/ui/logindialog.cpp:92-106`（按钮/表单）、`:40-70`（固定 min width 620）
- 触发：800×480 主窗口/低分辨率笔记本首次登录；620 宽对话框加系统边框仍可用但左右余量很小；密码不可见切换缺失。
- 影响：可操作但输入体验差，用户容易输错密码。
- 修复建议：最小宽度改为 520 或按 screen availableGeometry 自适应；密码行增加显示/隐藏按钮；保持现有 Tab 顺序。
- 预估：0.25 天。

## P2（体验/一致性/维护）

### P2-1 播放器顶部信息栏只有 260px 宽的小面板，与全宽控制条不一致

- 文件：`app/src/ui/playerpage.cpp:797-800`
- 现象：`topWidth = qMin(qMax(260, topBar sizeHint), maxTopWidth)`；1280 宽时 topBar 仅 260×76，像一个悬浮卡片，控制条却全宽，视觉上与 qEmby 顶部信息条不一致。
- 建议：topBar 改为与控制条同宽（或至少 50% 宽），取消小卡片式不透明背景，仅保留标题/集信息；若需不透明命中区，透明区域也应保持整个 top bar 可点。
- 预估：0.25 天。

### P2-2 设置页有「界面」分组但左侧 5 分类没有「界面」入口

- 文件：`app/src/ui/settingspage.cpp:126-130`（分类：服务器/播放/媒体库/存储/关于）、`:239-252`（界面分组在服务器分类下）
- 现象：用户从「服务器」进入能看到「界面」，但左侧没有对应类别；切换分类时界面组跟随服务器显示，信息架构不一致。
- 建议：把「界面」并入「服务器」分组标题下，或新增「通用/界面」分类；保持 qEmby 分类命名。
- 预估：0.25 天。

### P2-3 设置页可见「Mock（本地演示数据）」和临时存储文案，属于开发残留

- 文件：`app/src/ui/settingspage.cpp:375`、`:518`（Mock（本地演示数据））、`tokenstore.cpp:42`（QSettings 临时存储明文）
- 触发：设置 → 界面/存储；普通用户不需要看到 Mock/临时明文，容易误解为客户端只能演示。
- 建议：正式构建隐藏 mock 开关或改为「本地演示数据」并放到关于/诊断；Token 存储只显示 DPAPI/QSettings。
- 预估：0.25 天。

### P2-4 焦点环 2px 会改变 36×36 固定按钮内部布局

- 文件：`app/src/ui/style/dark.qss:1087-1092`（所有 QPushButton focus 2px），配合 `playerpage.cpp:65` / `:349` 固定 36×36
- 现象：键盘 focus 时 border 增厚可能挤压图标/文字，播放器图标按钮和标题栏按钮发生 1–2px 位移。
- 建议：focus 使用 inset outline，或固定按钮预留 focus 2px padding。
- 预估：0.1 天。

### P2-5 代码注释仍写「5px 边距」，与 T56 的 6px 命中区不一致

- 文件：`app/src/ui/playerpage.cpp:34`、`:213`、`:463`、`:572`
- 现象：维护者按 5px 注释排查会误判实际 6px `kPlayerResizeHit`；不影响运行。
- 建议：统一注释为「6px 命中区，布局 margin 0」。
- 预估：0.05 天。

### P2-6 首页 section 卡片标题无省略号/换行策略

- 文件：`app/src/ui/homeview.cpp:151-159`、QSS `HomeSectionCardTitle`
- 现象：窄列 76px 时较长中文标题直接裁切，无 elide/tooltip（只有 meta 有 overview tooltip）。
- 建议：标题 QLabel 开启 elide 或 wordWrap + tooltip；卡宽动态变化时保持一致。
- 预估：0.2 天。

## 用户已报回归项复核

| 用户问题 | 当前状态 | 证据/备注 |
|---|---|---|
| 窗口边框/圆角 | 已修（T54/T56），待实机 DWM | `windowframe.cpp` DWM 直角 + MARGINS 0；Win10 DWM 失败仅日志 |
| 全屏退出残留/无缩放光标 | 已修（T46/T57） | `restoreWindowedStateFromFullscreen()` 强制 showNormal + NoState；T57 回归 |
| 全屏/播放模式退出侧边栏变形 | 已修（T47/T57） | splitter/sidebar 240→240、clamp 180–320；T57 restoreWindowedChrome |
| 标题栏 close/min/max 不见 | 已修（T57） | 320×180→800×480，TitleBar 宽度=窗口宽，三按钮 rect 在窗口内；selftest |
| 控制条只有图标中心能点 | 已修（T51/T55） | 外扩 6px + 不透明 overlay + 四角命中 4/4 |
| 控制条 320 小窗口 | **未解决（P0-2）** | 320×180 控制条固定宽度/高度溢出 |
| 首页缩放/滚动 | **未解决（P0-1）** | 800/960 section content 宽度 1022 vs viewport 546/706，横向滚动关闭 |

## 修复批次建议

1. **第一批 P0**：P0-1 HomeView 宽高约束 + P0-2 播放模式 320 compact/min size；两项都影响用户最小窗口体验。
2. **第二批 P1**：首页错误/加载态、设置 800 媒体库行、卡片/播放器 a11y。
3. **第三批 P2**：视觉一致性、mock 文案、焦点/注释。

> 本报告只审查，未修改任何 UI 代码；所有行号基于 `a28e44b` 当前工作树。
