# GitHub Actions 构建说明（docs/ci.md）

> 文档：docs/ci.md · 维护：release-engineer（T11）· 2026-11-04
> 仓库：`hiccup90/emby-pc`（默认 private）· 工作流：`.github/workflows/windows-build.yml`

---

## 1. 这个工作流做什么

在 **windows-2022** 上构建 Windows x64 Release 包，产物是**免安装 zip** 和 **Inno Setup 安装包（exe）** 两种 artifact。

> 为什么不是 windows-latest：2026-11 起 `windows-latest` 指向 Windows Server 2025 + VS 2026 镜像，不再自带 VS 2022，CMake 的 `Visual Studio 17 2022` 生成器不可用；Qt 6.8 的 `win64_msvc2022_64` 与 VS 2022 匹配，因此固定 `windows-2022`。

| 步骤 | 内容 |
|---|---|
| Checkout | 拉取仓库 |
| Install Qt | `jurplel/install-qt-action@v4` 安装 Qt 6.8.x MSVC 2022 x64（`QT_ROOT_DIR`） |
| Fetch libmpv | `app/scripts/fetch-libmpv.ps1` 下载 shinchiro mpv-dev x86_64 包，**URL 固定 + SHA-256 校验**，安装 `libmpv-2.dll` 与 `include/mpv` |
| Configure | `cmake -S app -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=<Qt>` |
| Build | `cmake --build build --config Release --parallel` |
| Emby tests | 单独构建 `app/src/emby`（`EMBY_BUILD_TESTS=ON`）并跑 `ctest` |
| Package | `app/scripts/package-windows.ps1 -SkipBuild`：windeployqt + 复制 libmpv-2.dll + 打 zip |
| Installer | `choco install innosetup` 后校验 exe VERSIONINFO，用 `installer/emby-pc.iss` 编译 `dist/emby-pc-<version>-setup.exe` |
| Upload | 上传 artifact **emby-pc-win64**（免安装 zip）+ **emby-pc-installer**（安装包），均保留 **30 天** |

触发条件：`push` 到 `main`，或在 Actions 页面点 **Run workflow**（`workflow_dispatch`）。

- Actions 页面：<https://github.com/hiccup90/emby-pc/actions/workflows/windows-build.yml>
- 单次构建约 5~10 分钟（Qt 缓存命中会更快）。

---

## 2. 下载构建产物（给验收人）

### 方式 A：网页

1. 打开 <https://github.com/hiccup90/emby-pc/actions>，进入 **windows-build**。
2. 选择最近一次绿色（成功）的 run。
3. 页面底部 **Artifacts** 区域选择下载：
   - **emby-pc-installer**：`emby-pc-<version>-setup.exe`，双击安装（推荐普通用户；步骤见 [docs/packaging.md](packaging.md)）。
   - **emby-pc-win64**：免安装 zip，解压即用（验收/便携）。
4. （免安装版）解压后目录里应有：`emby-pc.exe`、`libmpv-2.dll`、`platforms\qwindows.dll`、`WINDOWS-ACCEPTANCE.md`、`README-运行.txt`。

### 方式 B：gh CLI

```bash
# 查看最近 run
gh run list --repo hiccup90/emby-pc --workflow windows-build

# 下载指定 run 的 artifact 到 dist/
gh run download <run-id> --repo hiccup90/emby-pc -n emby-pc-win64 -D dist
gh run download <run-id> --repo hiccup90/emby-pc -n emby-pc-installer -D dist

# 或者直接下载最近一次成功的 artifact
gh run download --repo hiccup90/emby-pc -n emby-pc-win64 -D dist
gh run download --repo hiccup90/emby-pc -n emby-pc-installer -D dist
```

> artifact 保留 30 天；过期后重新触发一次 workflow 即可。

---

## 3. 手动触发 / 重跑

```bash
# 触发 main 分支的最新 workflow（需要 workflow_dispatch，本工作流已开启）
gh workflow run windows-build.yml --repo hiccup90/emby-pc --ref main

# 重新运行某次失败的 run
gh run rerun <run-id> --repo hiccup90/emby-pc
```

也可以在 Actions 页面右侧点 **Re-run all jobs**。

## 4. 如何改版本号

当前版本号出现在三处，发布时保持一致：

| 位置 | 说明 |
|---|---|
| `app/CMakeLists.txt` 的 `project(emby-pc VERSION x.y.z ...)` | **当前 0.1.0**；驱动 zip 命名、exe VERSIONINFO、安装包文件名（CI 自动读取） |
| `app/src/main.cpp` 的 `app.setApplicationVersion("0.1.0")` | 应用内显示/日志版本 |
| `app/scripts/package-windows.ps1 -Version x.y.z` | 可选，覆盖上面自动读取的版本（只影响 zip 文件名） |

改完后提交并 push，CI 会自动产出 `dist/emby-pc-<version>-win64.zip`；若只是想打一个特定版本号、不改代码，可在本地运行：

```powershell
powershell -ExecutionPolicy Bypass -File app\scripts\package-windows.ps1 -QtPrefix C:\Qt\6.8.3\msvc2022_64 -Version 0.1.1
```

如需给 release 打 tag，建议格式 `v0.1.1`（工作流当前不监听 tag；需要的话可给 `on.push.tags` 增加 `v*`）。

---

## 5. 如何升级 libmpv

libmpv 版本固定在 workflow 顶部的 `env` 中：

```yaml
env:
  LIBMPV_URL: "https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/20261104/mpv-dev-x86_64-20261104-git-46787be250.7z"
  LIBMPV_SHA256: "5ae0aaa85b2e690e9c574a44e10a86f3d5fe4b406ef6dfa684f5ab25e6bf6306"
```

升级步骤：

1. 打开 shinchiro 发布页 <https://github.com/shinchiro/mpv-winbuild-cmake/releases>，选最新的 `mpv-dev-x86_64-*.7z`（**dev** 包）。
2. 拿到下载 URL 与 SHA-256（release notes 里每个 asset 都带 sha256；也可本地 `Get-FileHash` 算）。
3. 同时更新：
   - `.github/workflows/windows-build.yml` 的 `LIBMPV_URL` / `LIBMPV_SHA256`；
   - `docs/libmpv-source.md` 的获取记录（跑本地 `fetch-libmpv.ps1` 会自动回写标记区）。
4. push 后看 CI 是否通过；通过后在 Windows 上跑一遍 [windows-acceptance.md](windows-acceptance.md) 的 S1~S5。

> 本地下载校验（推荐先跑一遍，避免 CI 反复失败）：
> ```powershell
> powershell -ExecutionPolicy Bypass -File app\scripts\fetch-libmpv.ps1 -Url "<新 URL>" -Sha256 "<新 SHA256>"
> ```

---

## 6. 失败时怎么看日志

1. Actions → 失败的 run → 左侧点失败的 job（**Windows x64 Release**）。
2. 展开红色步骤，看命令输出；或命令行拉完整日志：
   ```bash
   gh run list --repo hiccup90/emby-pc --workflow windows-build
   gh run view <run-id> --repo hiccup90/emby-pc --log-failed
   ```
3. 下载完整日志：Actions 页面右上 **⋯ → Download log archive**。

### 常见失败与处理

| 现象 | 原因 / 处理 |
|---|---|
| `fetch-libmpv.ps1 failed ... SHA-256 校验失败` | URL 与 SHA-256 不匹配（选了别的架构/别的日期，或上游文件被替换）。重新从 release 页复制同一 asset 的 URL 和 sha256 |
| `Invoke-WebRequest` 403/404 | shinchiro release 的 asset 名写错（注意日期/git commit 后缀）；或 GitHub 网络临时问题，重跑 |
| `找不到 Qt（windeployqt.exe）` | Qt 安装步骤失败或 `QT_ROOT_DIR` 为空；看 Install Qt 步骤日志，确认 `arch: win64_msvc2022_64` |
| cmake configure 失败 | 看 CMake 输出；常见是 Qt 版本/编译器不匹配（MSVC 2022 x64）、或 `CMAKE_PREFIX_PATH` 没指到 Qt 根目录 |
| 编译/link 失败 | 看具体 .cpp/链接错误；本地用 `cmake -S app -B build-windows -G "Visual Studio 17 2022" -A x64` 复现 |
| ctest 失败 | 单独跑 `build-emby\Release\emby_tests.exe --verbose`；单元测试不依赖真实 Emby 服务器（用本地假服务器） |
| `windeployqt` 失败 | 确认在 Runner 上有 VS 运行时；必要时把 `--compiler-runtime` 去掉（仅影响 VC 运行库打包） |
| artifact 里没有 zip | 看 Package 步骤输出；`dist/emby-pc-*-win64.zip` 未生成时检查 `package-windows.ps1` 的报错 |

---

## 7. 敏感信息约定（push 前必读）

本仓库是 **private**，但仍按公开仓库标准处理，**绝不允许**提交：

- 任何 AccessToken / API Key / 密码 / cookie；
- 内网服务器地址（如 10.x / 192.168.x 的真实地址）、真实服务器名、测试账号；
- 私钥/证书（`*.pem` / `*.key` / `*.pfx`）。

约定：

1. 凭据只通过环境变量（`EMBY_URL` / `EMBY_USER` / `EMBY_PASSWORD`）或本地临时配置传入；`live_probe.cpp` 只读环境变量、不打印 token/password。
2. 文档/截图中的地址用示例地址（如 `192.168.1.10`），不要用真实内网 IP。
3. 提交前扫描：
   ```bash
   # 在仓库根目录执行；有输出就逐条确认
   grep -rInE '10\.10\.10\.|192\.168\.|api_key=|X-Emby-Token:|AccessToken' --exclude-dir=.git --exclude-dir=qEmby --exclude-dir=build-linux .
   ```
4. 万一误提交：立即在 GitHub 撤销/删除仓库或使用 `git filter-repo` 清理历史，并**轮换**相关 token/密码。
5. 仓库不需要任何 GitHub Secrets 就能构建（libmpv 是公开下载 + SHA-256 校验），不要把任何 token 放进 Secrets 以外的文件。

---

## 8. 本地复现 CI（Windows 上）

```powershell
# 与 CI 等价的关键命令
powershell -ExecutionPolicy Bypass -File app\scripts\fetch-libmpv.ps1 -Url "<CI 里的 LIBMPV_URL>" -Sha256 "<CI 里的 LIBMPV_SHA256>"

cmake -S app -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
cmake --build build --config Release --parallel

cmake -S app/src/emby -B build-emby -G "Visual Studio 17 2022" -A x64 -DEMBY_BUILD_TESTS=ON -DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64
cmake --build build-emby --config Release --parallel
ctest --test-dir build-emby -C Release --output-on-failure

powershell -ExecutionPolicy Bypass -File app\scripts\package-windows.ps1 -QtPrefix C:\Qt\6.8.3\msvc2022_64 -BuildDir build -SkipBuild
```

> Linux 容器（本开发环境）只能验证编译，不产出 Windows 包；Windows 包以 Actions artifact 为准。

---

## 9. 构建记录

| 日期 | 提交 | Run | Artifact | 备注 |
|---|---|---|---|---|
| 2026-11-04 | 28a5537 | <https://github.com/hiccup90/emby-pc/actions/runs/41791815017> | emby-pc-win64 / emby-pc-win64.zip（72,186,321 字节） | 首次成功：修复 windows-2022 runner 与 videohost static_cast，全步骤通过（3m54s） |
| 2026-11-04 | a47403d | <https://github.com/hiccup90/emby-pc/actions/runs/41793756453> | emby-pc-win64 / emby-pc-win64.zip（72,186,309 字节） | T7 真实媒体库（ui 源码 + README + preview PNG），全步骤通过（4m24s） |
| 2026-11-04 | 5d702e6 | <https://github.com/hiccup90/emby-pc/actions/runs/41841471114> | emby-pc-win64 / emby-pc-win64.zip（72,200,677 字节） | T15 修复：Qt 控制条可见/窗口 720x480+size grip/VideoHost EnumChildWindows/导出诊断；全步骤通过（3m52s），供用户复测 |
| 2026-11-04 | db9fd51 | <https://github.com/hiccup90/emby-pc/actions/runs/41862539535> | emby-pc-win64 / emby-pc-win64.zip（artifact id 13341056549） | T17/T18 基础版：player mode 接管、mpv 输入转发（SetWindowSubclass/comctl32）、自动隐藏浮层；成功（排队后完成），可作为兜底 |
| 2026-11-04 | 88d1268 | <https://github.com/hiccup90/emby-pc/actions/runs/41867053696> | emby-pc-win64 / emby-pc-win64.zip（72,211,822 字节，artifact id 13342717652） | **T17/T18 最终版**：MBTN_MID 探针、音轨 serial 修正、player mode 隐藏 QMenuBar、VideoHost 去 windowsx.h（LOWORD/HIWORD）；全步骤通过（4m30s），供用户复测 |
| 2026-11-04 | 0c8cfcb | <https://github.com/hiccup90/emby-pc/actions/runs/41872144136> | emby-pc-win64 / emby-pc-win64.zip（72,214,946 字节，artifact id 13346200280） | **T19**：mpv 子窗口 WM_KEYDOWN/UP 转发 keydown/keyup + VK 映射 + F11/Esc + 去重；全步骤通过（4m39s），键盘焦点兜底版 |
| 2026-11-05 | 81c2751 | <https://github.com/hiccup90/emby-pc/actions/runs/41952931196> | emby-pc-win64 / emby-pc-win64.zip（72,221,356 字节，artifact id 13382555893） | **T20+T21+T22**：海报回退链/失败重试（T20）、继续观看刷新/Stopped 位置（T21）、Qt 原生鼠标+键盘转发（T22，删除 SetWindowSubclass/comctl32）；全步骤通过（4m32s），供用户复测 |
| 2026-11-05 | bbdc809 | <https://github.com/hiccup90/emby-pc/actions/runs/41957153193> | emby-pc-win64 / emby-pc-win64.zip（72,230,490 字节，artifact id 13384052192） | **T23+T24**：海报请求 in-flight 去重/并发 6/FIFO（T23）、首页继续观看卡片 0 高度修复（T24）；全步骤通过（3m37s），供用户复测 |
| 2026-11-05 | 80a798a | <https://github.com/hiccup90/emby-pc/actions/runs/41968294548> | emby-pc-win64 / emby-pc-win64.zip（72,235,727 字节，artifact id 13388133321） | **T25**：复用客户端 QNAM、15s transferTimeout + 最多 2 次重试、api_key 图片 URL、本地磁盘 1GB LRU 缓存、qnam/http2/disk 统计；全步骤通过（4m18s），供用户复测 |
| 2026-11-05 | 1756163 | <https://github.com/hiccup90/emby-pc/actions/runs/41972577073> | emby-pc-win64 / emby-pc-win64.zip（72,245,921 字节，artifact id 13390243094） | **T26+T27**：DWM 无边框窗口（dwmapi/user32/shell32）+ 自绘 TitleBar/窗口状态记忆；全步骤通过（4m19s），DWM API 编译链接通过，供用户实机验收 |
| 2026-11-05 | 442012c | <https://github.com/hiccup90/emby-pc/actions/runs/41975729698> | emby-pc-win64 / emby-pc-win64.zip（72,262,430 字节，artifact id 13391213270） | **T28+T29+T30**：设置页四分区（T28）、设置实时生效/关闭 Stopped 上报/进度 10s/硬件信息回填（T29）、最小尺寸 800x480（T30）；全步骤通过（4m32s），供用户实机验收 |
| 2026-11-05 | bf3580c | <https://github.com/hiccup90/emby-pc/actions/runs/41978904742> | emby-pc-win64 / emby-pc-win64.zip（72,266,074 字节，artifact id 13393331425） | **T31+T32**：播放模式 5px 视频边距/黑边/overlay 内缩（T31）、全屏按钮与上下集按钮/队列同步修复（T32）；全步骤通过（4m29s），供用户实机验收 |
| 2026-11-05 | 6d65e06 | <https://github.com/hiccup90/emby-pc/actions/runs/41981162658> | emby-pc-win64（72,270,708 B，id 13394461333）+ **emby-pc-installer**（emby-pc-0.1.0-setup.exe，49,846,383 B，id 13394321833） | **T33/M4-1**：VERSIONINFO/图标（rc.exe 编译通过）、Inno Setup 安装包（48.3MB，<150MB）、VERSIONINFO 校验通过；全步骤通过（5m41s），供用户安装验收 |
| 2026-11-05 | 8bb825f | <https://github.com/hiccup90/emby-pc/actions/runs/41983403476> | emby-pc-win64（72,285,698 B，id 13395516069）+ **emby-pc-installer**（emby-pc-0.1.0-setup.exe，49,863,861 B，id 13394689169） | **T34+T35+T36**：崩溃 dump/异常退出提示（T34）、播放页边缘缩放（T35）、上下集异步队列补全（T36）；全步骤通过（6m11s），供用户实机验收 |
| 2026-11-05 | 1e35b70 | <https://github.com/hiccup90/emby-pc/actions/runs/41988831288> | emby-pc-win64（72,318,179 B，id 13396999622）+ **emby-pc-installer**（emby-pc-0.1.0-setup.exe，49,888,053 B，id 13397516393） | **T38（含 T37 数据层）**：QSplitter 侧边栏 180-320/记忆、首页 4 section 空行隐藏、媒体库显示/排序设置；修复 215f8f9 的 C2065（T38 头文件全量提交）；全步骤通过（5m52s），供用户实机验收 |
| 2026-11-05 | dad2a55 | <https://github.com/hiccup90/emby-pc/actions/runs/42044622876> | emby-pc-win64（72,319,889 B，id 13423519522）+ **emby-pc-installer**（emby-pc-0.1.0-setup.exe，49,882,722 B，id 13423513423） | **T39+T40**：首页 5×18 section 懒加载/固定预览/标题跳转（T39）、删除 QMenuBar/演示徽标/VideoHost 说明文字（T40）、preview 14 张与报告同步；全步骤通过（6m0s），供用户实机验收 |
| 2026-11-06 | ee6744d | <https://github.com/hiccup90/emby-pc/actions/runs/42150288059> | emby-pc-win64（72,320,447 B，id 13471528549）+ **emby-pc-installer**（emby-pc-0.1.0-setup.exe，49,888,689 B，id 13471634911） | **T43**：qEmby 视觉 token（tokens.h）+ dark.qss 全套重写 + qEmby SVG 参考素材/ATTRIBUTION、qemby-ui-reference.md、ui-polish-plan.md；全步骤通过（4m28s），供用户看基础观感 |
| 2026-11-06 | f7d0c9e | <https://github.com/hiccup90/emby-pc/actions/runs/42153306344> | emby-pc-win64（72,323,217 B，id 13473222747）+ **emby-pc-installer**（emby-pc-0.1.0-setup.exe，49,891,475 B，id 13473083056） | **T44**：首页 2:3 海报 rails+查看全部、媒体库网格、详情 hero、登录页、标题残留修复；全步骤通过（5m55s），供用户看页面观感 |

包内容（各次一致，54 个条目，解压约 171MB）：
emby-pc-0.1.0-win64/ 下有 emby-pc.exe、libmpv-2.dll、Qt6Core/Gui/Widgets/Network/Svg.dll、platforms/qwindows.dll、tls/、imageformats/、translations/、WINDOWS-ACCEPTANCE.md、LIBMPV-SOURCE.md、README-运行.txt。

> 后续每次 push 到 main 或手动 Run workflow 都会重新产出同名 artifact（emby-pc-win64，保留 30 天）；下载方式见第 2 节。
