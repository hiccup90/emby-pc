# libmpv 来源与许可记录

> 文档：docs/libmpv-source.md · 维护：release-engineer（T9）· 初始模板 2026-11-04
> 用途：记录 Windows 版 libmpv 的确切来源、版本、SHA-256 与许可说明，保证构建可复现、可审计。

---

## 1. 当前使用的 libmpv

> 本节由 `scripts/fetch-libmpv.ps1` 自动更新（标记区内的内容会被覆盖，请勿手工编辑标记区）。

<!-- fetch-libmpv:begin -->
| 项 | 值 |
|---|---|
| 包文件名 | mpv-dev-x86_64-20261104-git-46787be250.7z |
| 版本/构建 | mpv-dev-x86_64-20261104-git-46787be250 |
| 下载 URL | https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/20261104/mpv-dev-x86_64-20261104-git-46787be250.7z |
| 获取日期 | 2026-11-04 |
| SHA-256 | 5ae0aaa85b2e690e9c574a44e10a86f3d5fe4b406ef6dfa684f5ab25e6bf6306 |
| 安装路径 | app/libs/libmpv |
<!-- fetch-libmpv:end -->

### 1.1 安装后的目录结构

```text
app/libs/libmpv/
├── libmpv-2.dll          # 运行时动态库（随 emby-pc.exe 一起分发）
├── include/mpv/          # 编译用头文件：client.h / render.h / render_gl.h / stream_cb.h 等
└── README.md             # 原有说明
```

### 1.2 校验方法

```powershell
Get-FileHash .\app\libs\libmpv\libmpv-2.dll -Algorithm SHA256
```

> 注意：上面记录的是**下载压缩包**的 SHA-256（用于确认下载完整、与记录一致）。
> 解压出的 `libmpv-2.dll` 也可以单独再算一次，用于和别的机器比对。

---

## 2. 从哪里获取

推荐使用 **shinchiro** 维护的 Windows 构建（mpv 官方 Installation 页面列出的第三方构建之一）：

- 发布页：<https://github.com/shinchiro/mpv-winbuild-cmake/releases>
- 需要的文件：`mpv-dev-x86_64-<日期>-git-<commit>.7z`（**dev** 包，内含 `libmpv-2.dll` 和 `include/mpv`）
- 不要下载 `mpv-x86_64-*.7z`（那是带 mpv.exe 的完整播放器包，不含开发头文件；dll 可能同名但缺头文件，本脚本会校验）
- 如 CPU 较新，也可用 `mpv-dev-x86_64-v3-*.7z`（x86-64-v3 指令集，性能略好，但要求 Haswell/Excavator 之后）；**默认用基础 x86_64 版本，兼容性优先**

### 2.1 一键获取

```powershell
# 在仓库根目录执行（CI 使用同一 URL + SHA-256，见 .github/workflows/windows-build.yml）
powershell -ExecutionPolicy Bypass -File app\scripts\fetch-libmpv.ps1 `
  -Url "https://github.com/shinchiro/mpv-winbuild-cmake/releases/download/20261104/mpv-dev-x86_64-20261104-git-46787be250.7z" `
  -Sha256 "5ae0aaa85b2e690e9c574a44e10a86f3d5fe4b406ef6dfa684f5ab25e6bf6306"

# 只安装、不更新本文档：
powershell -ExecutionPolicy Bypass -File app\scripts\fetch-libmpv.ps1 -Url "<url>" -Sha256 "<该 asset 的 SHA-256>" -SkipDocUpdate
```

脚本会：下载 → 计算并打印 SHA-256 → 解压 → 把 `libmpv-2.dll` 和 `include/mpv` 复制到
`app/libs/libmpv/` → 把包名/URL/日期/SHA-256 写回本文档 1 节的标记区。

- 传入 `-Sha256` 时会强制校验下载/本地文件的 SHA-256，不一致立即非 0 退出；CI 必须传，本地建议也传。
- 网络受限时可先手工下载压缩包，再执行 `fetch-libmpv.ps1 -Url "D:\Download\mpv-dev-x86_64-*.7z"` 传本地文件路径（脚本会跳过下载，仍计算并校验 SHA-256）。
- `.7z` 解压优先用 7-Zip（`7z.exe`），没有时尝试 Windows 自带的 `tar.exe`（Windows 10 1803+ 基于 libarchive，可读 7z）；都没有会给出明确错误。
- 下载失败、包内找不到 `libmpv-2.dll` / `include/mpv` 时脚本以非 0 退出并打印原因。

---

## 3. 许可说明（重要）

| 项 | 说明 |
|---|---|
| mpv / libmpv | mpv 主体采用 **GPLv2+**（GNU GPL v2 或更高版本）。libmpv 也提供 **LGPLv2.1+** 构建选项（`--enable-lgpl`），但会关闭部分 GPL-only 组件。 |
| 本记录使用的构建 | shinchiro 的 mpv-dev 构建按 **GPLv2+** 分发（第三方构建，未做独立法律审查）。 |
| 分发义务 | 如果随 Emby PC 分发 GPL 版 libmpv，需要遵守 GPLv2+：提供对应源代码（或书面 offer）、保留版权与许可声明、允许再分发/反向工程等。闭源商业分发应改用 LGPL 构建并遵守 LGPL 条款（动态链接、提供替换库的能力、附带许可证文本）。 |
| 第三方依赖 | libmpv/FFmpeg 及其编解码库还包含各自的许可（FFmpeg 可为 LGPL/GPL，取决于编译选项；另有 libass、libplacebo 等）。发布前需汇总第三方许可清单。 |
| Qt | Qt 6 使用 LGPLv3（或商业许可），随包需附带 Qt 许可文本与 LGPL 合规说明。 |

> 结论：**MVP 内部测试阶段**可先用 shinchiro GPL 构建验证功能；**对外发布前**必须由项目负责人确认分发方式（GPL 开源 vs LGPL 构建 + 商业许可），并把许可文本随安装包分发。

---

## 4. 升级流程

1. 打开 shinchiro 发布页，选新的 `mpv-dev-x86_64-*.7z`，复制下载链接。
2. 运行 `fetch-libmpv.ps1 -Url <新链接>`（自动更新本文档记录）。
3. 重新构建并跑 Windows 验收清单：[windows-acceptance.md](windows-acceptance.md)（至少 S1~S5 + 统计面板）。
4. 验收通过后，把本文档的记录与版本号一起提交（注意：`app/libs/libmpv/libmpv-2.dll` 体积较大，是否入库按仓库策略决定；默认不入库，由脚本获取）。

### 4.1 版本历史

| 日期 | 包文件名 | SHA-256（前 16 位） | 备注 |
|---|---|---|---|
| 2026-11-04 | mpv-dev-x86_64-20261104-git-46787be250.7z | 5ae0aaa85b2e690e | CI/本地首次获取（shinchiro 20261104）；历史版本可手工追加到本表 |
