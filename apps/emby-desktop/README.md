# Emby Desktop（重写原型）

本文记录已确认的产品范围、当前实现状态，以及本地和 Windows CI 的验证方式。应用仍处于 prototype 阶段：已接入 Emby 登录、影库、详情、播放与进度上报，UI 通过 libmpv 播放真实媒体。2026-11-09 已在 Windows 实机连真实 Emby 服务器走过一轮验收清单，结果见“当前实现状态”里的验证范围。

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
- Emby 能力由 `src-tauri/crates/emby-api` 提供：登录、会话持久化、视图与条目查询、`PlaybackInfo`、进度上报和图片 URL。`src-tauri/src/emby.rs` 暴露同名 Tauri 命令 `emby_login`、`emby_logout`、`emby_session`、`emby_views`、`emby_items`、`emby_item`、`emby_playback_target`、`emby_report_playback`、`emby_image_url`。access token 不作为命令返回值下发前端，应用自己的日志也不写它；播放和图片 URL 仍通过 `api_key` 查询参数携带它，因此 libmpv 文件日志默认关闭（见“如何测试播放”末尾）。
- HWND 层叠和控制条窗口已在 Windows 实机上正常工作（见“当前实现状态”里的验证范围）；多显示器、不同 DPI 缩放下的行为仍需实机确认，不应把当前原型结构当作最终定案。

## 当前实现状态

- 前端播放层位于 `src/player/`：`contract.ts`、`tauri.ts`、`usePlayer.ts`、`useVideoSync.ts`、`PlayerPage.tsx`。
- 控制条是独立的透明窗口 `controls`（`controls.html`；代码在 `src/controls/` 和 `src-tauri/src/overlay.rs`），叠在视频底部。播放中 2.5 秒无操作自动隐藏，鼠标在视频上移动或点击时重新出现；音轨/字幕菜单在控制条上方展开。
- mpv 子窗口会吞掉鼠标事件，所以“点击视频激活窗口”和“双击视频切换全屏”由 `overlay.rs` 的光标轮询线程实现，不依赖网页里的点击事件。
- 界面位于 `src/screens/`（登录、影库、详情）并由 `src/emby/` 调用 Tauri 命令：登录页提交服务器地址、用户名和密码；影库展示服务器返回的视图、条目和“继续观看”；详情页的播放按钮调用 `emby_playback_target`，拿到直链后交给播放页。
- 播放中每 10 秒上报一次进度；暂停/继续、seek、播放结束和离开播放页也会上报。登出会删除本地会话并要求重新登录。
- 会话持久化在 Tauri `app_config_dir()` 下的 `session.json`。服务器地址会归一化：没有 scheme 时补 `http://`，并去掉尾部 `/`。收到 401/403 时清除会话并要求重新登录。当前会话是明文 JSON 文件，尚未使用系统密钥库。
- 播放 URL 使用 Emby 的 `api_key` 查询参数鉴权，因此不向 mpv 注入 header；图片 URL 也经 `emby_image_url` 返回带 `api_key` 的绝对地址。
- 验证范围：
  - Windows CI（见下文）每次运行 `cargo test -p emby-mpv`、`cargo test -p emby-api`、`tsc -b` 和 `tauri build --no-bundle`。2026-11-10 的 run `42718300778`（commit `7eecc7a`）中 emby-mpv 63 个、emby-api 44 个单测全部通过。
  - 前端没有自动化测试。早期曾用 Playwright + Tauri IPC mock 手动走通“登录 → 影库 → 详情 → 播放 → 上报 → 登出”，脚本没有入库。
  - 2026-11-09 在 Windows 实机连真实 Emby 服务器走过一轮验收清单，发现 4 个问题：点视频不激活窗口、双击视频不切全屏、展开音轨/字幕菜单闪屏、控制条显示时窗口边角无法缩放。均已在 `5f2bebe` 修复并复测通过。

## 如何测试播放

以下步骤是 Windows 实机验收清单，不是已验证结果。若还没有可用 artifact，先按“Windows CI”一节手动触发一次构建。

1. 从 GitHub Actions 页面下载 artifact，或在本仓库执行：

   ```sh
   gh run download <run-id>
   ```

   默认 artifact `emby-desktop-windows-x64-<sha>` 只含 `emby-desktop.exe`。第一次安装还需要 `libmpv-2.dll`：手动触发时勾选 `bundle_libmpv`（命令行加 `-f bundle_libmpv=true`），会另外产出含 exe 和 DLL 的 `emby-desktop-windows-x64-full-<sha>`。
2. 确认 `emby-desktop.exe` 与 `libmpv-2.dll` 位于同一目录，两者缺一不可。libmpv 版本没变时，之后更新只需替换 exe。
3. 运行 `emby-desktop.exe`。构建未签名，Windows 可能弹出 SmartScreen 提示；仅在确认产物来自本项目 CI 后继续。
4. 启动后进入登录页，填写服务器地址（如 `https://<server>` 或 `<host>:<port>`；省略 scheme 时按 `http://` 处理）、用户名和密码，点击登录。登录成功后会话写入 `session.json`；若提示登录失效，需要重新登录。
5. 在影库选择条目（也可从“继续观看”进入），打开详情页并点击播放。应用会向服务器请求直链，目前只请求 DirectPlay/DirectStream；需要转码的条目会提示无法直接播放。
6. 观察视频是否出现在黑色视频区域，并检查播放/暂停、seek、音量、静音、音轨/字幕切换，以及控制条收起后能否找回。同时观察续播是否从上次位置开始，以及 Emby 服务端是否记录了进度（均待实测）。

若没有画面或播放失败：

- libmpv 文件日志默认关闭。需要时在 PowerShell 里设置环境变量，再从同一个窗口启动（`1`、`true`、`yes`、`on` 都可以）：

  ```powershell
  $env:EMBY_MPV_LOG = "1"
  .\emby-desktop.exe
  ```

  日志写到 `%TEMP%\emby-desktop\mpv-<pid>.log`，是判断 mpv 加载和播放错误的主要依据。它包含完整播放 URL，也就包含 `api_key`：用完删掉，发给别人前先删掉 `api_key=` 后面的值。
- 若界面提示“播放器组件缺失或无法加载”，先确认 `libmpv-2.dll` 与 exe 同目录；也可用环境变量 `EMBY_MPV_DLL` 指向 DLL 的绝对路径。
- 实机验收记录见“当前实现状态”里的验证范围；上面的步骤用于复测和收集新问题。

## 本地开发命令

在 `apps/emby-desktop/` 中：

```sh
pnpm install
pnpm dev
pnpm build
pnpm tauri dev
```

- `pnpm dev` 只启动 Vite，浏览器中为 UI 预览，播放命令不会发送，控件仅更新本地假状态。
- `pnpm dev:mock` 在浏览器里模拟 Tauri IPC（`src/dev/mockTauri.ts`），用一个虚构的 Emby 影库跑完整主窗口：登录、影库、详情、播放页都能点。URL 参数可切换场景：`?session=none`（登录页，密码填 `wrong` 会失败）、`?state=loading|error|empty`、`?resume=none`、`?posters=none`。这个模块只在 `--mode mock` 下加载，正式构建不包含。
- `pnpm build` 执行 `tsc -b && vite build`。
- `pnpm tauri dev` 需要本机 Rust/Tauri 工具链；原生视频嵌入仅在 Windows 可用。
- 编译和 Rust 单测统一放在 Windows CI 上跑，本地 Linux 容器不装 Rust 工具链。有 Rust 环境时，可以在本地检查两个不依赖 Tauri 的 crate：

  ```sh
  cd src-tauri
  cargo test -p emby-mpv
  cargo test -p emby-api
  cargo check -p emby-api --features tls --target x86_64-pc-windows-gnu
  cargo check -p emby-mpv --target x86_64-pc-windows-gnu
  ```

  单测数量以 CI 日志为准。

## Windows CI（仅手动触发）

workflow 为 `.github/workflows/emby-desktop-windows.yml`，只配置了 `workflow_dispatch`，提交不会自动触发。手动启动命令：

```sh
gh workflow run emby-desktop-windows.yml --ref rewrite/tauri-emby
```

- 运行环境：`windows-2022`、Node 20、pnpm 9.15.4（`pnpm-lock.yaml` 已入库）、Rust stable MSVC x64。
- 构建步骤包括 `pnpm exec tsc -b`、Rust 单测（步骤名 `Test workspace crates (if present)`，会运行 `cargo test -p emby-mpv` 与 `cargo test -p emby-api`）和 `pnpm tauri build --no-bundle`；不产 MSI。
- workflow 下载固定版本 libmpv `20261108`，校验 SHA-256 后把 `libmpv-2.dll` 复制到 exe 同目录。
- 默认 artifact `emby-desktop-windows-x64-<sha>` 只含 `emby-desktop.exe`，保留 3 天；加 `-f bundle_libmpv=true` 触发时，另外产出 `emby-desktop-windows-x64-full-<sha>`（exe + `libmpv-2.dll`，约 49 MB），保留 1 天。CI 通过只说明能编译、单测通过，不代表播放已验收。
- libmpv 来自 GPLv2+ 构建，分发 exe 与 DLL 前需确认许可证合规。

## 待验证与未决事项

- Windows 实机验证多显示器与不同 DPI 缩放、最小化/还原、控制条隐藏时的点击穿透，以及这些情况下控制条的位置与找回。2026-11-09 已确认：视频嵌入与播放、点击视频激活窗口、双击切换全屏、控制条显示时的边角缩放、音轨/字幕菜单展开不闪屏。2026-11-10 起控制条窗口固定为展开高度，菜单收起改用窗口区域（SetWindowRgn）裁剪，不再缩放窗口；选择音轨/字幕后不闪、收起状态下点控制条上方的视频仍能激活窗口，这两点待实机确认。
- 用真实 Emby 服务器验证登录、直链和图片加载，包括 https 与自签名证书场景；同时用真实 Emby 直链验证播放、seek、音轨/字幕切换，并确认硬解是否生效（`hwdec-current`）。
- 验证 `emby_playback_target` 返回的 `startSeconds` 是否按上次进度续播。
- 验证播放中每 10 秒、暂停/继续、seek、结束和离开页面时的进度上报，是否被 Emby 面板正确记录。
- 验证多用户登录、登出后切换账号或服务器时会话是否隔离，旧会话是否被正确清除。
- 评估 token 安全存储：当前 `session.json` 为明文 JSON，未使用 Windows 凭据管理器或系统密钥库。
- 验证退出时序：关闭窗口和其他应用退出路径触发时，mpv 应在父窗口销毁前完成清理。
- 实机评估内存与 GPU 使用情况。
- HDR10 是否纳入首版仍待确认；Dolby Vision 暂不做，不据此推定其他 HDR 能力。
