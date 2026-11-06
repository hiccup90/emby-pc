# 更新日志（CHANGELOG）

> 版本：v0.1.1-rc · 2026-11-06 · 维护：release-engineer
> 详细审查与 P0 依据：reports/full-audit.md

## v0.1.1-rc（2026-11-06，发布候选）

0.1.0 为内部构建基线；本版依据 B1 批修（full-audit 的 10 个代码 P0 + 1 个验收口径决策）产出首个对外发布候选。

### 安全 / 隐私（P0）
- A-P0-01 图片请求不再把 access token 拼进 URL（去掉 api_key 查询参数，仅用 Authorization 头）。
- A-P0-02 mpv.log / 诊断包不再记录 http-header-fields 中的 token（导出前脱敏 + 日志收敛；诊断包敏感词扫描 0 命中）。

### 播放 / 会话（P0）
- A-P0-05 剧集播放中 N/P 键可切上一集/下一集（走 PlayerController 队列，本地文件保留 mpv playlist 兜底）。
- A-P0-06 双击视频进入/退出应用全屏（不再由 mpv 子窗口处理）。
- A-P0-07 续播：进度 >95% 时从头播放；新增「自动续播/记忆播放位置」开关（设置 → 播放）。
- A-P0-08 播放错误统一处理：错误码/建议/重试（≤2 次）/返回/导出诊断；本地文件错误同样弹窗。
- A-P0-09 退出前等待最后一次 Stopped 进度上报，避免继续观看位置回退。
- A-P0-10 硬解失败自动降级 auto-safe 并提示「已回退软解/兼容模式」；设置页可选择硬解模式。

### 布局 / 交互（P0）
- A-P0-03 首页 800/960 宽不再横向溢出/裁切；预览卡按 viewport 自适应列数。
- A-P0-04 播放模式 320×180 启用 compact 布局（顶栏/控制条不重叠，次要按钮收进溢出菜单）。

### 验收口径（决策）
- A-P0-D1 保留 Qt 控制条，不使用 mpv OSC；docs/windows-acceptance.md 的 O-01~O-08 已改为 Qt 控制条验收（O-05 音量滑杆为 P1）。决策记录见 reports/decisions.md §5。

### 其他
- 安全 P1 加固：token 存储改用 Windows DPAPI（Linux fallback 混淆 + 权限 600）、401/403 清理会话、退出登录清空 Authorization、跨主机不附加鉴权头。
- 控制条/窗口回归：T51/T52/T53/T54/T55/T56/T57（命中区、320×180 最小尺寸、纯图标、DWM 直角、不透明 overlay、无边框全铺满、退出播放模式恢复 chrome）。
- 文档：新增 docs/release-checklist.md、CHANGELOG.md；README / 打包文档同步 v0.1.1；preview 截图更新。

### 版本信息
- 版本：0.1.0 → 0.1.1（CMake project VERSION / exe VERSIONINFO / 安装包文件名 / 关于页）。
- 产物：emby-pc-0.1.1-win64.zip、emby-pc-0.1.1-setup.exe（artifact：emby-pc-win64 / emby-pc-installer）。

### 已知限制
- Windows 实机 P0 验证项 V1（HDR10 不发灰）/ V2（DV P5/P8 不绿）需按 docs/windows-acceptance.md 的 S 组完成，失败不得发布。
- 未做代码签名，SmartScreen 可能提示未知发布者。
- token 存储的 Windows DPAPI 分支需 Windows 实机验证（Linux 容器只覆盖回退混淆文件）；ui::QSettingsTokenStore 仍为直接 UI/测试入口的旧明文实现，正式 app 入口统一走 main.cpp 的 SecureUiTokenStore。
