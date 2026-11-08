# Emby Desktop（重写原型）

本文记录已确认的产品范围、当前实现状态，以及本地和 Windows CI 的验证方式。应用仍处于 prototype 阶段：已接入 Emby 登录、影库、详情、播放与进度上报，UI 通过 libmpv 播放真实媒体；尚未在真实 Emby 服务器或 Windows 实机上完成验收。

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
- Emby 能力由 `src-tauri/crates/emby-api` 提供：登录、会话持久化、视图与条目查询、`PlaybackInfo`、进度上报和图片 URL。`src-tauri/src/emby.rs` 暴露同名 Tauri 命令 `emby_login`、`emby_logout`、`emby_session`、`emby_views`、`emby_items`、`emby_item`、`emby_playback_target`、`emby_report_playback`、`emby_image_url`。access token 不作为命令返回值下发前端，也不写日志；播放和图片 URL 仍通过 `api_key` 查询参数携带它。
- HWND 层叠、点击穿透、resize/DPI 等行为仍需 Windows 实机确认，不应把当前原型结构当作最终定案。

## 当前实现状态

- 前端播放层位于 `src/player/`：`contract.ts`、`tauri.ts`、`usePlayer.ts`、`useVideoSync.ts`、`PlayerPage.tsx`。播放页提供“播放地址”输入框，可直接打开任意直链 URL。
- 控制条位于视频下方，不覆盖画面，可收起为细栏；音轨/字幕菜单在控制条 dock 内展开，不落入视频区域。
- 界面位于 `src/screens/`（登录、影库、详情）并由 `src/emby/` 调用 Tauri 命令：登录页提交服务器地址、用户名和密码；影库展示服务器返回的视图、条目和“继续观看”；详情页的播放按钮调用 `emby_playback_target`，拿到直链后交给播放页。
- 播放中每 10 秒上报一次进度；暂停/继续、seek、播放结束和离开播放页也会上报。登出会删除本地会话并要求重新登录。
- 会话持久化在 Tauri `app_config_dir()` 下的 `session.json`。服务器地址会归一化：没有 scheme 时补 `http://`，并去掉尾部 `/`。收到 401/403 时清除会话并要求重新登录。当前会话是明文 JSON 文件，尚未使用系统密钥库。
- 播放 URL 使用 Emby 的 `api_key` 查询参数鉴权，因此不向 mpv 注入 header；图片 URL 也经 `emby_image_url` 返回带 `api_key` 的绝对地址。
- 播放页保留可折叠的“调试直链”输入框，用于绕过 Emby 单独验证“前端控制 → Tauri 命令 → libmpv”。
- 验证范围：`cargo test -p emby-api` 42 个单测通过；`cargo check -p emby-api --features tls --target x86_64-pc-windows-gnu` 通过；前端 `tsc -b`、`vite build` 通过；Playwright + Tauri IPC mock 走通“登录 → 影库 → 详情 → 播放 → 上报 → 登出”。真实播放、真实 Emby 服务器、HWND 层叠、DPI、全屏、硬解和退出行为均未在 Windows 实机验证。

## 如何测试播放

以下步骤是 Windows 实机验收清单，不是已验证结果。若还没有可用 artifact，先按“Windows CI”一节手动触发一次构建。

1. 从 GitHub Actions 页面下载 artifact `emby-desktop-windows-x64-<sha>`，或在本仓库执行：

   ```sh
   gh run download <run-id>
   ```

2. 解压后确认 `emby-desktop.exe` 与 `libmpv-2.dll` 位于同一目录，两者缺一不可；只复制 exe 无法加载播放内核。
3. 运行 `emby-desktop.exe`。构建未签名，Windows 可能弹出 SmartScreen 提示；仅在确认产物来自本项目 CI 后继续。
4. 启动后进入登录页，填写服务器地址（如 `https://<server>` 或 `<host>:<port>`；省略 scheme 时按 `http://` 处理）、用户名和密码，点击登录。登录成功后会话写入 `session.json`；若提示登录失效，需要重新登录。
5. 在影库选择条目（也可从“继续观看”进入），打开详情页并点击播放。应用会向服务器请求直链，目前只请求 DirectPlay/DirectStream；需要转码的条目会提示无法直接播放。
6. 观察视频是否出现在黑色视频区域，并检查播放/暂停、seek、音量、静音、音轨/字幕切换，以及控制条收起后能否找回。同时观察续播是否从上次位置开始，以及 Emby 服务端是否记录了进度（均待实测）。
7. 如果只想验证播放内核，在播放页展开折叠的“调试直链”，粘贴公开的 H.264 MP4 直链，或带 `api_key` 的 Emby 直链：

   ```text
   https://<server>/Videos/<item-id>/stream?static=true&api_key=<api-key>
   ```

   具体路径以服务端配置为准。
8. 点击“播放”，重复第 6 步检查播放控制。

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
- 本地 Linux 容器没有 Windows 运行环境，Rust 侧只做测试和交叉编译检查；Windows 原生构建由 CI 完成。已在 Linux 容器中执行：

  ```sh
  cd src-tauri
  cargo test -p emby-api
  cargo check -p emby-api --features tls --target x86_64-pc-windows-gnu
  cargo check -p emby-mpv --target x86_64-pc-windows-gnu
  ```

  `cargo test -p emby-api` 当前为 42 个单测通过。

## Windows CI（仅手动触发）

workflow 为 `.github/workflows/emby-desktop-windows.yml`，只配置了 `workflow_dispatch`，提交不会自动触发。手动启动命令：

```sh
gh workflow run emby-desktop-windows.yml --ref rewrite/tauri-emby
```

- 运行环境：`windows-2022`、Node 20、pnpm 9.15.4（`pnpm-lock.yaml` 已入库）、Rust stable MSVC x64。
- 构建步骤包括 `pnpm exec tsc -b`、Rust 单测（步骤名 `Test workspace crates (if present)`，会运行 `cargo test -p emby-mpv` 与 `cargo test -p emby-api`）和 `pnpm tauri build --no-bundle`；不产 MSI。
- workflow 下载固定版本 libmpv `20261108`，校验 SHA-256 后把 `libmpv-2.dll` 复制到 exe 同目录。
- artifact 名为 `emby-desktop-windows-x64-<sha>`，内含 `emby-desktop.exe` 与 `libmpv-2.dll`。最近一次成功 run 为 `42498677959`，只证明构建通过，不代表播放已验收。
- libmpv 来自 GPLv2+ 构建，分发 exe 与 DLL 前需确认许可证合规。

## 待验证与未决事项

- Windows 实机验证 HWND 层叠与点击穿透、resize、DPI、全屏、最小化和焦点行为，以及控制条隐藏与找回。
- 用真实 Emby 服务器验证登录、直链和图片加载，包括 https 与自签名证书场景；同时用公开直链和真实 Emby 直链验证播放、seek、音轨/字幕切换，并确认硬解是否生效（`hwdec-current`）。
- 验证 `emby_playback_target` 返回的 `startSeconds` 是否按上次进度续播。
- 验证播放中每 10 秒、暂停/继续、seek、结束和离开页面时的进度上报，是否被 Emby 面板正确记录。
- 验证多用户登录、登出后切换账号或服务器时会话是否隔离，旧会话是否被正确清除。
- 评估 token 安全存储：当前 `session.json` 为明文 JSON，未使用 Windows 凭据管理器或系统密钥库。
- 验证退出时序：关闭窗口和其他应用退出路径触发时，mpv 应在父窗口销毁前完成清理。
- 实机评估内存与 GPU 使用情况。
- HDR10 是否纳入首版仍待确认；Dolby Vision 暂不做，不据此推定其他 HDR 能力。
