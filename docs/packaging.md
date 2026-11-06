# Windows 安装包说明（docs/packaging.md）

> 任务：T33 / M4-1 · 维护：release-engineer · 更新：2026-11-05
> 产物：GitHub Actions artifact **emby-pc-installer**（emby-pc-<version>-setup.exe，保留 30 天）
> 约束：安装包 ≤150MB；干净 Windows 10/11 x64 可安装、有桌面/开始菜单快捷方式、卸载干净、升级保留用户数据。

---

## 1. 产物一览

| Artifact | 文件 | 用途 |
|---|---|---|
| emby-pc-installer | dist/emby-pc-<version>-setup.exe | **推荐给用户**：Inno Setup 安装包，安装到 C:/Program Files/Emby PC |
| emby-pc-win64 | dist/emby-pc-win64.zip | 免安装绿色版，解压即用（验收/便携场景） |

版本号来自 app/CMakeLists.txt 的 project(emby-pc VERSION x.y.z LANGUAGES CXX)，当前 **0.1.1**。
Windows 安装包与 exe 的 VERSIONINFO、产物文件名都由该版本号驱动。

## 2. 版本信息与图标（T33）

### 2.1 文件位置

| 文件 | 说明 |
|---|---|
| app/resources/win/version.rc | VERSIONINFO + 图标资源模板；含 @PROJECT_VERSION_*@ / @APP_ICON_PATH@ 占位符，由 CMake 的 configure_file() 替换 |
| app/resources/win/app.ico | 占位应用图标（16/32/48/256），exe 与安装包共用 |
| app/resources/win/make-icon.py | 图标生成脚本（纯 Python，无第三方依赖） |

CMake 只在 WIN32 下 enable_language(RC)、生成 build/win/version.rc 并编译进目标；Linux 构建完全不受影响。

### 2.2 修改版本号

1. 改 app/CMakeLists.txt 的 project(emby-pc VERSION x.y.z ...)；
2. 同步改 installer/emby-pc.iss 里 #ifndef MyAppVersion 的默认值（仅本机不带 /D 编译时用到，CI 会自动传）；
3. 重新跑 CI，exe 属性、安装包文件名、卸载列表版本都会更新。

> 版本号建议遵循 SemVer；升级到 1.0 前保持 0.x.y。

### 2.3 替换图标

```bash
# 修改 make-icon.py 里的配色/形状后重新生成
python3 app/resources/win/make-icon.py
ls -l app/resources/win/app.ico
```

也可以直接用设计稿导出的 .ico（含 16/32/48/256）覆盖 app/resources/win/app.ico。
安装包图标来自 [Setup] SetupIconFile=../app/resources/win/app.ico；exe 图标来自 version.rc 的 IDI_ICON1。

## 3. 本地构建安装包（Windows）

前置：Windows 10/11 x64、Qt 6.8.x MSVC 2022 x64、CMake 3.21+、Inno Setup 6（https://jrsoftware.org/isinfo.php）、已跑过 fetch-libmpv.ps1。

```powershell
# 1) 构建 + windeployqt + zip（生成 dist/emby-pc-<version>-win64/）
powershell -ExecutionPolicy Bypass -File app/scripts/package-windows.ps1 -QtPrefix C:/Qt/6.8.3/msvc2022_64

# 2) 读取版本号并编译安装包
$version = (Select-String -Path app/CMakeLists.txt -Pattern 'project\s*\(\s*emby-pc\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)').Matches[0].Groups[1].Value
$stage = (Resolve-Path "dist/emby-pc-$version-win64").Path
& "C:/Program Files (x86)/Inno Setup 6/ISCC.exe" "/DMyAppVersion=$version" "/DSourceDir=$stage" installer/emby-pc.iss

# 3) 产物
Get-ChildItem dist/emby-pc-*-setup.exe
```

> 只打安装包、不重建 exe 时，第 1 步可加 -SkipBuild（前提是 build/Release/emby-pc.exe 已存在）。

## 4. Inno Setup 脚本要点（installer/emby-pc.iss）

- **AppId 固定**：{{EDD10CCD-4E71-4192-9B5D-7B3A071BB464}}，升级/卸载靠它识别，**不要改**。
- 默认安装目录：{autopf}/Emby PC（64 位 Windows 上即 C:/Program Files/Emby PC）。
- 快捷方式：开始菜单组 + 可选桌面快捷方式（默认勾选）。
- 卸载项：控制面板「应用」中显示 Emby PC <version>；UninstallDisplayIcon 指向 exe。
- 用户数据：%LOCALAPPDATA%/emby-pc（日志/缓存/登录信息）默认 **保留**；卸载时 GUI 会询问是否删除，静默卸载传 /DELETEUSERDATA 才会删除。
- 压缩：lzma2/ultra64 + SolidCompression=yes，当前安装包约 60~70MB（≤150MB 目标）。
- 无额外安装前检查（不依赖 VC++ 运行库——windeployqt --compiler-runtime 已把运行库打进目录）。

## 5. CI 流程（.github/workflows/windows-build.yml）

```text
Checkout -> Install Qt -> Fetch libmpv -> Configure/Build -> Emby tests
-> Package（windeployqt + libmpv + zip）
-> Install Inno Setup（choco install innosetup）
-> Verify exe VERSIONINFO（ProductName/ProductVersion/FileDescription）
-> Build installer（ISCC /DMyAppVersion /DSourceDir）
-> Upload artifact emby-pc-win64 + emby-pc-installer
```

- 安装包构建失败会直接让 run 红；日志里看 ISCC failed。
- CI 会校验 exe 的 ProductVersion 与 app/CMakeLists.txt 一致，防止忘记改版本。
- 安装包大小 >150MB 会抛错（保护目标）。
- artifact 下载方式见 docs/ci.md 第 2 节。

## 6. 安装/升级/卸载验证清单

### 6.1 干净安装（Windows 10/11 x64）

1. 从 Actions 下载 emby-pc-installer，双击 emby-pc-<version>-setup.exe。
2. 安装位置默认 C:/Program Files/Emby PC；完成后开始菜单有 Emby PC，桌面有快捷方式（若勾选）。
3. 启动应用：能打开登录/首页，无缺少 DLL 报错；%LOCALAPPDATA%/emby-pc/emby-pc/app.log 正常生成。
4. 右键 exe -> 属性 -> 详细信息：产品名 Emby PC、文件版本/产品版本与发布版本一致、图标正确。

### 6.2 升级

1. 用旧版本安装包安装并登录、改设置（如控制条延时）、播放一次留下缓存。
2. 直接运行新版本安装包覆盖安装（不要先卸载）。
3. 确认：登录态/设置/缓存仍在；版本号已更新；开始菜单/桌面快捷方式指向新 exe。
4. 本次发布门禁：**0.1.0 → 0.1.1 覆盖安装**后，登录态、服务器地址、设置、缓存与继续观看位置均保留；关于页显示 0.1.1。

### 6.3 卸载

1. 设置 -> 应用 -> Emby PC -> 卸载；选择「否」保留用户数据。
2. 确认安装目录已删除、快捷方式已删除、应用列表无残留。
3. 确认 %LOCALAPPDATA%/emby-pc 仍在；重新安装后登录态/设置仍可恢复。
4. 再卸载一次并选择「是」：确认 %LOCALAPPDATA%/emby-pc 被删除（静默卸载用 /DELETEUSERDATA）。

### 6.4 大小/签名

- release gate：setup.exe ≤150MB（CI 会检查）。
- 当前**未做代码签名**，Windows SmartScreen 可能提示「未知发布者」；正式发布前再补签名（自签名或 EV 证书）。

## 7. 常见问题

| 现象 | 处理 |
|---|---|
| ISCC.exe not found | 本机未装 Inno Setup 6；或路径不是 C:/Program Files (x86)/Inno Setup 6，改脚本/命令里的路径 |
| 安装包编译报 SourceDir 不存在 | 先跑 package-windows.ps1 生成 dist/emby-pc-<version>-win64；或检查 /DSourceDir 路径带空格时加引号 |
| 安装后启动提示缺 Qt6*.dll | 确认用 package-windows.ps1 的 staged 目录打安装包（windeployqt 已部署），不要直接拿 build/Release 打 |
| 升级后设置丢失 | 检查是否在卸载时选择了删除用户数据；或 %LOCALAPPDATA% 被清理 |
| SmartScreen 拦截 | 未签名安装包的预期行为；点「更多信息 -> 仍要运行」，或后续补签名 |
| 安装包过大 | 检查 staged 目录是否混入调试符号/pdb；当前 libmpv-2.dll 约 116MB，压缩后约 60~70MB 总包 |

## 8. 相关文档

- CI/artifact 下载：docs/ci.md
- Windows 实机验收：docs/windows-acceptance.md
- libmpv 来源与许可：docs/libmpv-source.md
