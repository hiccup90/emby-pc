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