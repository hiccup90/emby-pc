# Emby Desktop（重写原型）

本文记录首版已确认的产品范围、暂定架构和当前原型状态。这里的应用是 prototype，不代表 Emby 或原生播放器已经接通。

## 产品要求（已确认）

- 新应用基于干净 main 开始；旧 Qt 项目保持原样，重写从零实现，不复用旧项目代码。
- 目标平台为 Windows 10/11；产品只支持 Emby。
- UI 使用 React + TypeScript + Tauri 2；播放引擎目标为 libmpv。
- 首版范围：登录、媒体库浏览、媒体详情、播放与暂停、seek、音量、续播/进度，以及基本音轨和字幕选择。
- 不做下载、直播、音乐或弹幕。播放器 control bar 应美观且可隐藏；不要求控件覆盖视频画面。
- Dolby Vision 暂不做。尽量使用 mpv 默认行为和配置；HDR10 是否属于首版要求尚未确认，不在此作额外保证。
- 不迁移旧配置或凭据；新应用使用新的配置命名空间。

## 架构边界（暂定）

目标技术方向是 React + TypeScript 前端、Tauri 2 桌面外壳和 libmpv 播放引擎。具体的 Tauri IPC、Emby 通信、Rust 与 C/C++ 代码职责，以及 libmpv 集成边界都还没有定案；不要把原型结构当作最终架构。

Windows 上视频 HWND 与渲染路线也未定。确定实现方案前，需要针对目标 Windows 版本验证窗口、渲染和控件交互。

## 当前实现状态（prototype）

当前 `apps/emby-desktop/` 只有本地静态 UI demo 和最小 Tauri scaffold：前端样例数据位于 `src/data/demo.ts`，Rust 入口位于 `src-tauri/src/main.rs`。没有 Emby 服务连接，也没有 libmpv 集成。

界面中的登录只是模拟流程：提交后进入本地样例影库，不保存服务器地址、不验证凭据，也不发起网络连接。播放器画面和控件是交互演示；播放、进度、音量、音轨和字幕状态不是实际媒体播放或真实媒体轨道操作。

现有 Windows workflow `.github/workflows/emby-desktop-windows.yml` 运行 `pnpm tauri build --no-bundle`；这是无 bundle 的构建，不是安装包产出，也不代表真实播放功能已验证。

## 本地开发命令（建议，未运行）

在本目录 `apps/emby-desktop/` 中，现有 package scripts 和 Tauri 配置对应以下建议命令：

```sh
pnpm install
pnpm dev
pnpm build
pnpm tauri dev
```

这些命令仅供后续开发者尝试；本文未安装依赖或运行构建。Tauri 开发运行还需要相应的本机 Rust、Tauri 和 Windows 开发环境。

## 待验证与未决事项

- 在 Windows 10/11 实机验证 control bar 隐藏与找回，以及 resize、fullscreen、DPI 和 focus 行为。
- 接入真实 Emby stream 后验证登录、媒体信息和播放链路；确认默认 mpv 行为与所需的基本音轨/字幕控制。
- 实机评估内存与 GPU 使用情况。
- 定案 IPC、Emby 通信、Rust/C++ 与 libmpv 的职责边界，以及 Windows 视频 HWND/render 方案。
- HDR10 是否纳入首版仍待确认；Dolby Vision 暂不做，不据此推定其他 HDR 能力。
