# T38 侧边栏自适应 + 首页多分栏 + 媒体库显示设置

> 任务：task-38（依赖 T37 HomeSection 数据层） · 日期：2026-11-05
> 范围：app/src/ui/mainwindow.*、homeview.*、sidebar.*、settingspage.*、style/dark.qss、demo_main.cpp、本报告。

## 1. 侧边栏 QSplitter

- 主窗口 central widget 改为 `QSplitter#MainSplitter`：左 `Sidebar`、右 MainColumn；
  `setChildrenCollapsible(false)`、`handleWidth=4`，内容侧 stretch=1。
- Sidebar 去掉 `setFixedWidth(224)`，改为 `minimumWidth=180`、`maximumWidth=320`；
  启动读取 QSettings `ui/sidebarWidth`（默认 224，clamp 180–320），show 后事件循环补一次尺寸。
- `splitterMoved` 与 `closeEvent` 保存当前侧边栏宽度到 `ui/sidebarWidth`；
  播放/全屏时 splitter handle 宽度设 0，视频区才能铺满客户区；退出恢复 4px。
- 隐藏媒体库时 MainWindow 维护 `m_sidebarSectionIndexes`，侧边栏索引映射回完整 m_sections，避免索引错位。

## 2. 首页多分栏

- `HomeView` 新增 `setHomeSections(const QList<HomeSection>&)`，按 T37 返回顺序渲染：
  - 每行 `HomeSectionRow` + `HomeSectionTitle` + 横向 `QScrollArea` + `HomeSectionCard`；
  - 继续观看卡片点击发 `playRequested`，其余行发 `itemActivated`；
  - 空 section 不加入布局（`sectionCount/visibleSectionCount` 只统计可见行）；
  - 整页 `HomeSectionsScroll` 可纵向滚动，复用现有 ThumbnailCache/深色主题；
  - `visibleHomeItems/visibleHomeItemIds` 供 MainWindow 的 `visibleItems/libraryIds` 使用。
- legacy `setResumeItems/setRecentItems` 路径保留给 mock/旧测试，通过 `HomeViewStack` 与 sections 页切换。
- `loadRealHome` 的 fetchHomeSections 回调改为 `m_home->setHomeSections(sections)`，不再把 per-library 行丢弃。

## 3. 设置页「媒体库」分区
- `SettingsPage::setLibrarySections()` 接收 MainWindow 的 Views；动态生成行：
  - `SettingsSidebarVisibleCheck`、`SettingsHomeVisibleCheck`；
  - `SettingsLibraryUpButton` / `SettingsLibraryDownButton` 调整首页库顺序；
  - 侧边栏隐藏时首页勾选禁用，tooltip「需先勾选侧边栏显示」。
- 写入 QSettings：
  - `sidebar/hiddenLibraries`（QStringList view id）；
  - `home/hiddenLibraries`；
  - `home/sectionOrder`（保留 resume/recent，再按 UI 顺序追加 `lib:<viewId>`）。
- 发出 `libraryDisplaySettingsChanged()`；MainWindow 清 `clearHomeSectionsCache()`、过滤侧边栏并重新加载首页。

## 4. 自测

- `QT_QPA_PLATFORM=offscreen emby-ui-demo --selftest`：**165 项 ALL PASS**（假服务器/offscreen）。
- T38 新增覆盖：
  - HomeView 4 个 section（含 1 空库）→ 顺序正确渲染 3 行、空行隐藏；
  - QSplitter 存在、Sidebar 180–320、`ui/sidebarWidth=300` 启动恢复；
  - 设置页库行、侧边栏隐藏后首页勾选禁用、QSettings 键写入、下移调整 sectionOrder；
  - 设置页隐藏库后 Sidebar sections 数量 2→1；
  - 关闭保存 Sidebar 宽度后新窗口恢复 250。
- Linux app Debug/Release 构建、Windows CI 由 release-engineer 统一验证。

## 5. 已知限制

- mock 首页仍保留旧 resume/recent 两段（兼容旧截图/测试）；真实登录后按 T37 HomeSection 渲染多行。
- 视频区 5px 黑边由 T31 处理；播放模式 splitter handle 隐藏，退出恢复。
- 媒体库排序只调整首页库行；resume/recent 顺序由 T37 `home/sectionOrder` 决定。

## 6. T39 性能优化（实机卡顿）

- **首页 section 懒加载**：`setHomeSections()` 先只建标题行，首屏只构建视口附近 section（5×18 测试：built=3/5，其余滚动时增量创建）；未构建行保留 230px 占位高度，滚动位置稳定。
- **滚动增量**：`HomeSectionsScroll` verticalScrollBar valueChanged → 80ms 单次 QTimer 防抖 → `buildVisibleSections()`，只构建进入视口 + 预取范围的 section。
- **缩略图 DPR**：HomeSectionCard/ResumeCard 使用 `devicePixelRatioF()` 计算物理像素尺寸请求缓存；缩略图 ready 信号合并为 120ms 防抖刷新，避免每张图触发整页遍历。
- **构建计时**：日志新增 `HomeView::setHomeSections ... elapsedMs`、`HomeView::buildVisibleSections ... elapsedMs/builtTotal/thumbnailMisses`；5 section×18 项 offscreen 首屏 182ms（目标 <1500ms，无 GPU 环境）。
- **splitter 拖动**：splitterMoved 不再每次写 QSettings，改 220ms 防抖保存；拖动只触发 QSplitter 尺寸变化，不调用 HomeView/侧边栏重建。
- **QSS/特效**：检查无 QGraphicsDropShadowEffect、无频繁 setStyleSheet；主题只在启动设置一次。
- **自测**：`emby-ui-demo --selftest` 173 项 ALL PASS；T39 新增懒加载 built< total、首屏构建 <1500ms、窗口缩放不重建、滚动后增量构建到 5/5。

## 7. T40 首页交互与界面精简

### 7.1 首页行交互

- 仅 `resume` 行保留横向 `QScrollArea`；`recent`/`library` 行改为 `HomeSectionPreview` + `QGridLayout`，不横向滚动。
- 预览数量按窗口宽度自适应：6–8 张（`qBound(6, viewportWidth/190, 8)`），超出不显示「更多」；窗口 resize 时只增删/重排已有预览卡片，不重建 section。
- 非 resume 标题使用 `HomeSectionTitle`，可点击；HomeView 发出 `sectionActivated(int kind, QString libraryId)`。
- MainWindow：Recent 标题 → `showRecentPage()`（LibraryView 本地显示最近添加条目）；Library 标题 → 按 viewId 查找并进入对应媒体库页。

### 7.2 QMenuBar / 快捷键

- UI 层不再创建/显示菜单栏；app `PlayerController` 不再调用 `menuBar()`，只把 QAction 挂到主窗口：Ctrl+O 打开文件、Ctrl+Shift+O 打开文件夹、Ctrl+Q 退出、`ExportDiagnosticsAction` 供设置页诊断导出。
- 帮助菜单的打开日志目录/关于已由设置页按钮承担；`main.cpp` 的 `installHelpMenu()` 删除。
- `ui::MainWindow` offscreen 断言 `findChild<QMenuBar*>() == nullptr`。

### 7.3 说明性文字/演示徽标

- 删除 PlayerPage `演示模式` 徽标；VideoHost 占位只保留播放图标，不再显示「VideoHost 占位/attachVideoSurface/--wid」说明。
- MainWindow 顶部数据源状态、侧边栏「演示数据 · 本地 Mock」、详情「演示数据」标签、设置页 Mock 说明、Resume 空状态长文案均已移除/缩短为「未连接」「暂无内容」等必要状态。
- 主题、卡片、进度条样式保持不变；截图更新到 `app/src/ui/preview/`。

### 7.4 自测

- `emby-ui-demo --selftest`：**173 项 ALL PASS**。
- T40 新增覆盖：无 QMenuBar、最近添加标题点击跳转（stack=1）、非继续观看行 6–8 预览且无横向滚动、点击标题发出 sectionActivated、无演示徽标/VideoHost 说明文字。
- T39 性能测试仍保留：懒加载 built< total、5×18 首屏 182ms、resize 不重建、滚动增量到 5/5。
