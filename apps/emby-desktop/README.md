# Emby Desktop（重写原型）

本文记录已确认的产品范围、当前实现状态，以及本地和 Windows CI 的验证方式。应用仍处于 prototype 阶段：UI 已接上真实 mpv 播放内核，但 Emby 服务尚未接入。

## 产品要求（已确认）

- 新应用在不继承旧历史的独立 orphan 分支（`rewrite/tauri-emby`）上从零实现；旧 Qt 项目与 `main` 保持原样。
- 目标平台为 Windows 10/11；产品只支持 Emby。
- UI 使用 React + TypeScript + Tauri 2；播放引擎使用 libmpv。
- 首版范围：登录、媒体库浏览、媒体详情、播放与暂停、seek、音量、续播/进度，以及基本音轨和字幕选择。
- 不做下载、直播、音乐或弹幕。播放器 control bar 应美观且可隐藏；不要求控件覆盖视频画面。
- Dolby Vision 暂不做。尽量使用 mpv 默认行为和配置；HDR10 是否属于首版要求尚未确认，不在此作额外保证。
- 不迁移旧配置或凭据；新应用使用新的配置命名空间。

## 架构边界（当前实现）

- Tauri 2 负责窗口、IPC 和生命周期；React 负责界面与播放控件；`src-tauri/crates/emby-mpv` 负责动态加载 `libmpv-2.dll` 并管理 mpv 实例。
- libmpv 通过 `--wid` 嵌入主窗口内的 Win32 子窗口 `EmbyVideoHost`；前端通过 `video_sync` 同步视频区域的坐标、尺寸和 DPI。
- Tauri 命令包括 `video_sync`、`player_open`、`player_set_paused`、`player_seek`、`player_set_volume`、`player_set_muted`、`player_set_track`、`player_stop`、`player_snapshot`、`player_toggle_fullscreen`、`player_subscribe`；播放状态快照通过 Tauri Channel 推送。
- Emby API 封装、凭据存储和进度上报尚未实现。HWND 层叠、点击穿透、resize/DPI 等行为仍需 Windows 实机确认，不应把当前原型结构当作最终定案。

## 当前实现状态

- 前端播放层位于 `src/player/`：`contract.ts`、`tauri.ts`、`usePlayer.ts`、`useVideoSync.ts`、`PlayerPage.tsx`。播放页提供“播放地址”输入框，可直接打开任意直链 URL。
- 控制条位于视频下方，不覆盖画面，可收起为细栏；音轨/字幕菜单在控制条 dock 内展开，不落入视频区域。
- 播放链路已接通，可手动粘贴直链 URL 验证“前端控制 → Tauri 命令 → libmpv”。Emby 登录、影库和详情仍使用本地演示数据；登录是模拟流程，没有凭据存储，也没有进度上报。
- 验证范围：Linux 容器内 `tsc -b`、`vite build` 通过；`cargo check -p emby-mpv --target x86_64-pc-windows-gnu` 通过；前端用 headless 浏览器 + Tauri IPC mock 验证过布局和命令参数。真实播放、HWND 层叠、DPI、全屏、硬解和退出行为均未在 Windows 实机验证。

## 如何测试播放

以下步骤是 Windows 实机验收清单，不是已验证结果。若还没有可用 artifact，先按“Windows CI”一节手动触发一次构建。

1. 从 GitHub Actions 页面下载 artifact `emby-desktop-windows-x64-<sha>`，或在本仓库执行：

   ```sh
   gh run download <run-id>
   ```

2. 解压后确认 `emby-desktop.exe` 与 `libmpv-2.dll` 位于同一目录，两者缺一不可；只复制 exe 无法加载播放内核。
3. 运行 `emby-desktop.exe`。构建未签名，Windows 可能弹出 SmartScreen 提示；仅在确认产物来自本项目 CI 后继续。
4. 进入播放页，在“播放地址”中粘贴直链，例如公开的 H.264 MP4 直链，或带 `api_key` 的 Emby 直链：

   ```text
   https://<server>/Videos/<item-id>/stream?static=true&api_key=<api-key>
   ```

   具体路径以服务端配置为准。
5. 点击“播放”，观察视频是否出现在黑色视频区域，并检查播放/暂停、seek、音量、静音、音轨/字幕切换，以及控制条收起后能否找回。

若没有画面或播放失败：

- 查看 `%TEMP%\emby-desktop\mpv-*.log`（实际文件名为 `mpv-<pid>.log`），日志是判断 mpv 加载和播放错误的主要依据。
- 若界面提示“播放器组件缺失或无法加载”，先确认 `libmpv-2.dll` 与 exe 同目录；也可用环境变量 `EMBY_MPV_DLL` 指向 DLL 的绝对路径。
- 当前没有 Windows 实机通过记录；上述操作只用于收集结果，不能据此声称播放已验证。

## 本地开发命令

在 `apps/emby-desktop/` 中：

```sh
pnpm install
pnpm dev
pnpm build
pnpm tauri dev
```

- `pnpm dev` 只启动 Vite，浏览器中为 UI 预览，播放命令不会发送，控件仅更新本地假状态。
- `pnpm build` 执行 `tsc -b && vite build`。
- `pnpm tauri dev` 需要本机 Rust/Tauri 工具链；原生视频嵌入仅在 Windows 可用。
- 容器/CI 没有 Windows 运行环境，Rust 侧只能做编译检查。已在 Linux 容器中执行：

  ```sh
  cd src-tauri
  cargo check -p emby-mpv --target x86_64-pc-windows-gnu
  ```

## Windows CI（仅手动触发）

workflow 为 `.github/workflows/emby-desktop-windows.yml`，只配置了 `workflow_dispatch`，提交不会自动触发。手动启动命令：

```sh
gh workflow run emby-desktop-windows.yml --ref rewrite/tauri-emby
```

- 运行环境：`windows-2022`、Node 20、pnpm 9.15.4（`pnpm-lock.yaml` 已入库）、Rust stable MSVC x64。
- 构建步骤包括 `pnpm exec tsc -b`、`cargo test -p emby-mpv` 和 `pnpm tauri build --no-bundle`；不产 MSI。
- workflow 下载固定版本 libmpv `20261108`，校验 SHA-256 后把 `libmpv-2.dll` 复制到 exe 同目录。
- artifact 名为 `emby-desktop-windows-x64-<sha>`，内含 `emby-desktop.exe`（约 8.8 MB）与 `libmpv-2.dll`（约 116 MB）。最近一次成功 run 为 `42488574394`（约 3 分钟），只证明构建通过，不代表播放已验收。
- libmpv 来自 GPLv2+ 构建，分发 exe 与 DLL 前需确认许可证合规。

## 待验证与未决事项

- Windows 实机验证 HWND 层叠与点击穿透、resize、DPI、全屏、最小化和焦点行为，以及控制条隐藏与找回。
- 用公开直链和真实 Emby 直链验证播放、seek、音轨/字幕切换，并确认硬解是否生效（`hwdec-current`）。
- 验证退出时序：关闭窗口和其他应用退出路径触发时，mpv 应在父窗口销毁前完成清理。
- 接入 Emby 后验证登录、凭据存储、媒体信息、播放进度上报和续播链路。
- 实机评估内存与 GPU 使用情况。
- HDR10 是否纳入首版仍待确认；Dolby Vision 暂不做，不据此推定其他 HDR 能力。
