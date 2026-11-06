# 发布判定清单（G1~G8）

> 版本：v0.1.1-rc · 2026-11-06 · 维护：release-engineer
> 用法：发布前逐项勾选；G1~G7 全 PASS 才可执行 G8。证据填 CI run URL / artifact / 截图路径。

## G1 构建与自动化测试
- [ ] Linux 干净 Release 构建 exit 0（全新 build 目录）
- [ ] Linux 干净 Debug 构建 exit 0
- [ ] emby ctest 全 PASS
- [ ] emby-ui-demo --selftest ≥200 项 ALL PASS，0 FAIL
- [ ] 编译 0 warning（-Wall -Wextra -Wpedantic）
证据：

## G2 安全与隐私扫描
- [ ] 仓库敏感信息扫描 0 命中（token / API key / 内网 IP / 测试账号）
- [ ] 诊断包（app.log / mpv.log / diagnostics.txt）不含 token / Authorization 明文
- [ ] 图片/媒体 URL 不含 api_key 等 token 参数
- [ ] 提交内容不含日志、dist/build、qEmby、pdb
证据：

## G3 Windows 实机 P0 验收
- [ ] O 组（Qt 控制条 O-01~O-04、O-06~O-08）全 PASS
- [ ] P 组（P-01~P-04）、W 组 P0、L 组（日志/诊断）全 PASS
- [ ] S1 HDR10：不发灰 / 不偏色（对照同机 mpv CLI）
- [ ] S2 DV P5 / S3 DV P8.1/8.4：不绿屏 / 紫屏
- [ ] 键盘快捷键（空格 / ←→ / ↑↓ / N P / I / F / Esc）全 PASS
- [ ] Windows DPAPI token 存储 roundtrip + 旧明文 QSettings 迁移（Linux 容器未覆盖 DPAPI 分支）
证据：

## G4 播放内核与降级
- [ ] hwdec-current 以 d3d11va 开头且无 -copy；current-vo=gpu-next；gpu-api=d3d11
- [ ] 强制不可用时自动回退 auto-safe / 软解并有提示
- [ ] 4K 播放 10 分钟 dropped frames ≤10 或记录基线
证据：

## G5 打包与安装
- [ ] CI 产出 emby-pc-win64（zip）与 emby-pc-installer（setup.exe）
- [ ] setup.exe ≤150MB；exe VERSIONINFO ProductVersion=0.1.1、ProductName=Emby PC
- [ ] 干净机安装：开始菜单/桌面快捷方式、启动、登录
- [ ] 升级 0.1.0→0.1.1 覆盖安装：登录态/设置/缓存保留
- [ ] 卸载：程序目录/快捷方式清理；用户数据默认保留；/DELETEUSERDATA 生效
证据：

## G6 关键回归
- [ ] 首页 800/960/1280 无横向裁切；继续观看可见
- [ ] 播放模式 320×180 compact 不重叠；退出恢复 800×480 与标题栏
- [ ] 续播：98% 从头、关闭开关从头、10 分钟位置续播 ±2s
- [ ] N/P 切集、双击全屏、错误重试 ≤2、退出后 Stopped 位置更新
- [ ] 实体库 1050415 可播放（无 401，日志无 token）
证据：

## G7 文档与版本一致性
- [ ] README / CHANGELOG / acceptance / decisions / packaging 内的版本与 0.1.1 一致
- [ ] O 组验收口径为 Qt 控制条；decisions.md 记录决策
- [ ] docs/ci.md 已记录本次成功 run 与 artifact
- [ ] CHANGELOG 的 P0 列表与实际修复一致
证据：

## G8 发布
- [ ] main 最新 commit 的 windows-build run 全绿
- [ ] artifacts 已上传且未过期（30 天）
- [ ] 打 tag v0.1.1（如需）并附 release notes（CHANGELOG 摘要）
- [ ] 通知用户下载 artifact 并附验收清单
证据：

## 当前状态（2026-11-06）
| Gate | 状态 | 备注 |
|---|---|---|
| G1 | 待 B1 三车道合入后执行 | selftest 目标 ≥200 |
| G2 | 待执行 | |
| G3 | 待 Windows 实机 | V1/V2 阻塞 |
| G4 | 待 Windows 实机 | |
| G5 | 待新版本 CI | |
| G6 | 待 Windows 实机 | |
| G7 | 部分完成 | 文档已改，版本待提交 |
| G8 | 未开始 | |
