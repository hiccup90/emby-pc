# Emby PC（Windows 桌面客户端）

自研 Emby 桌面客户端：**Qt 6 Widgets + libmpv（--wid 嵌入）+ mpv OSC**，目标平台 Windows 10/11 x64。
核心目标：HDR10 / 杜比视界（P5/P8）片源正确 tone-map 到 SDR（**不发灰、不绿屏**）、ASS 特效字幕、低 GPU 占用（D3D11VA 零回拷）。

> 当前状态：M1 播放内核已完成（本地文件播放 / OSC / 键盘 / 硬解配置），M2 Emby 接入进行中。
> 验收清单见 [docs/windows-acceptance.md](docs/windows-acceptance.md)。

## 快速开始（Windows）

### 方式 1：下载 CI 构建产物（推荐）

1. 打开仓库 **Actions → windows-build**，选择最近一次成功的 run。
2. 下载 artifact（二选一，均保留 30 天）：
   - **emby-pc-installer**：安装包 `emby-pc-<version>-setup.exe`，双击安装（推荐；桌面/开始菜单快捷方式、可从「应用」卸载），见 [docs/packaging.md](docs/packaging.md)。
   - **emby-pc-win64**：免安装 zip，解压到不含中文/空格的目录，双击 `emby-pc.exe`（验收/便携）。
3. 首次启动前确认已安装 VC++ 运行库（安装包已内置；免安装版由 windeployqt 附带）。

### 方式 2：本地构建

前置：Windows 10/11 x64、Qt 6.8.x MSVC 2022 x64（或 MinGW）、CMake 3.21+、[可选] 7-Zip。

```powershell
# 1) 获取 libmpv（固定 URL + SHA-256，见 docs/libmpv-source.md）
powershell -ExecutionPolicy Bypass -File app\scripts\fetch-libmpv.ps1 `
  -Url "https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/20261104/mpv-dev-x86_64-20261104-git-46787be250.7z" `
  -Sha256 "5ae0aaa85b2e690e9c574a44e10a86f3d5fe4b406ef6dfa684f5ab25e6bf6306"

# 2) 构建 Release 并打包（产物 dist\emby-pc-<version>-win64.zip）
powershell -ExecutionPolicy Bypass -File app\scripts\package-windows.ps1 -QtPrefix C:\Qt\6.8.3\msvc2022_64

# 3) 构建安装包（需 Inno Setup 6，详见 docs\packaging.md）
#    iscc /DMyAppVersion=0.1.0 "/DSourceDir=dist\emby-pc-0.1.0-win64" installer\emby-pc.iss
```

> 网络受限时，可先手工下载 mpv-dev 的 .7z，再把本地路径传给 `fetch-libmpv.ps1` 的 `-Url`。

### 方式 3：Linux 容器只做编译验证

容器/CI 无 Windows、无 GPU，只验证能编译链接：

```bash
./app/scripts/build-linux.sh
# 或指定目录/配置：BUILD_DIR=build-check BUILD_TYPE=Release ./app/scripts/build-linux.sh
```

> --wid 嵌入、D3D11VA 硬解、HDR/DV tone-map、mpv OSC 仍需 Windows 实机验证。

## 日志与验收

- 应用日志：启动后菜单 **帮助 → 打开日志目录**（Windows 通常在 %LOCALAPPDATA%\emby-pc\emby-pc\）。
- `app.log`：应用日志（5MB × 3 轮转）；`mpv.log`：mpv 全量日志（打开视频后生成）。
- 完整分步验收清单（构建/播放/OSC/键盘/统计面板/素材矩阵/窗口/日志收集）：
  [docs/windows-acceptance.md](docs/windows-acceptance.md)。
- CI 说明与常见失败处理：[docs/ci.md](docs/ci.md)。

## 目录结构

```text
.
├── app/                     # 应用工程
│   ├── CMakeLists.txt       # 根工程：Qt6 Widgets + libmpv + 可选 emby/ui 模块
│   ├── src/
│   │   ├── main.cpp         # 入口：app.log 初始化、帮助菜单、创建 ui::MainWindow
│   │   ├── mainwindow.*     # PlayerController：VideoHost + MpvPlayer 接线
│   │   ├── mpvplayer.*      # libmpv 封装（--wid、硬解/色彩/OSC、mpv.log）
│   │   ├── videohost.*      # 视频宿主窗口（--wid 目标）
│   │   ├── emby/            # Emby/Jellyfin API 客户端（Qt Network 静态库 + 自测）
│   │   └── ui/              # Qt Widgets UI：侧边栏/媒体库/详情/播放/设置/登录
│   ├── scripts/
│   │   ├── build-linux.sh       # Linux 编译验证
│   │   ├── fetch-libmpv.ps1     # 下载/校验/安装 libmpv
│   │   └── package-windows.ps1  # Windows Release 打包（windeployqt + zip）
│   ├── resources/win/       # T33：version.rc + app.ico + make-icon.py
│   └── libs/libmpv/         # mpv 头文件（DLL 由脚本获取，不入库）
├── installer/               # Inno Setup 安装包脚本（emby-pc.iss）
├── docs/
│   ├── requirements.md      # 需求规格 / 验收标准
│   ├── windows-acceptance.md# Windows 实机验收清单
│   ├── libmpv-source.md     # libmpv 版本/URL/SHA-256/许可记录
│   └── ci.md                # GitHub Actions 使用说明
├── .github/workflows/       # windows-build.yml（windows-latest 构建产物）
├── reports/                 # 各阶段分析/验证报告
└── mockups/ + app/src/ui/preview/  # UI 截图
```

## 文档

| 文档 | 内容 |
|---|---|
| [docs/requirements.md](docs/requirements.md) | MVP 需求、P0/P1、验收标准 |
| [docs/windows-acceptance.md](docs/windows-acceptance.md) | Windows 实机分步验收（含 S1~S5 素材矩阵） |
| [docs/libmpv-source.md](docs/libmpv-source.md) | libmpv 来源、SHA-256、GPL/LGPL 许可说明 |
| [docs/ci.md](docs/ci.md) | Actions 构建、下载 artifact、改版本、排错 |
| [docs/packaging.md](docs/packaging.md) | 安装包构建、版本号/图标修改、安装/升级/卸载验证 |
| [reports/](reports/) | GPU/窗口/架构/集成/联调分析报告 |

## CI

- 工作流：`.github/workflows/windows-build.yml`（windows-2022 + VS 2022 + Qt 6.8 msvc2022_64）
- 触发：push 到 `main`，或 Actions 页面 **Run workflow**（workflow_dispatch）
- 产物：artifact **emby-pc-win64**（免安装 zip）+ **emby-pc-installer**（Inno Setup 安装包），均保留 30 天

## 安全约定

- **仓库中不允许出现任何 token、密码、内网服务器地址或测试账号**；凭据只通过环境变量/本地配置传入。
- `app/src/emby/tests/live_probe.cpp` 仅从环境变量读取凭据，不打印 token/password。
- 细节见 [docs/ci.md](docs/ci.md) 的「敏感信息」一节。

## 许可

本项目代码许可待定；libmpv/Qt/FFmpeg 等第三方组件的分发义务见 [docs/libmpv-source.md](docs/libmpv-source.md)。
