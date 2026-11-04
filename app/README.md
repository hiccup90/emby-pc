# emby-pc · 播放内核 + UI 骨架（方案 A）

Qt 6 Widgets + libmpv（`--wid` 嵌入）+ mpv OSC；正式 UI 壳来自 `src/ui`（侧边栏 + 媒体库 / 详情 / 播放页）。
目标：验证「HDR/杜比视界片源正确 tone-map 到 SDR、ASS 特效字幕、键盘控制、播放统计」。

## 依赖

- Windows 10/11 x64
- Qt 6.5+（MSVC 2022 x64 或 MinGW 构建）
- CMake 3.21+
- libmpv：见 `libs/libmpv/README.md`

## 构建

### Windows

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/6.8.0/msvc2022_64
cmake --build build --config Release
```

产物在 `build/Release/emby-pc.exe`，`libmpv-2.dll` 会被自动复制过去。

MinGW 构建类似，把 `-G` 换成 `"MinGW Makefiles"` 即可。

### Linux（仅编译验证）

容器/CI 里没有 Windows 和 GPU，只验证代码能编译链接通过：

```bash
./scripts/build-linux.sh              # 默认输出到 build-linux/（Debug）
BUILD_DIR=build-check BUILD_TYPE=Release ./scripts/build-linux.sh
```

> Linux 只做语法/链接验证；`--wid` 嵌入、D3D11 硬解、HDR/DV tone-map 仍需在 Windows 实机验证。

## 运行

启动后进入 `ui::MainWindow` 首页（媒体库目前为 mock 演示数据）。

- `文件 → 打开文件…`（Ctrl+O）播放单个本地文件
- `文件 → 打开文件夹…`（Ctrl+Shift+O）把整个文件夹当播放列表（用于测试上一集/下一集）
- 打开文件后自动切到播放页，视频由 libmpv 嵌入 `VideoHost`；真实播放时隐藏 Qt 控制条，改用 mpv OSC

## 快捷键（mpv 窗口有焦点时）

| 键 | 功能 |
|---|---|
| 空格 | 播放 / 暂停 |
| ← / → | 快退 / 快进 5 秒 |
| Shift+← / Shift+→ | 快退 / 快进 1 秒 |
| ↑ / ↓ | 音量 +/- |
| n / p | 下一集 / 上一集 |
| i | 播放统计（mpv stats） |
| f | 全屏（通过 script-message 转给 Qt） |
| Esc | 退出全屏 |

Qt 菜单里的快捷键在 Qt 窗口有焦点时同样可用。

## mpv 关键配置（代码里已设置，见 `src/mpvplayer.cpp`）

```ini
vo=gpu-next
gpu-api=d3d11
gpu-context=d3d11
hwdec=d3d11va
target-colorspace-hint=no
target-trc=srgb
target-prim=bt.709
tone-mapping=auto
hdr-compute-peak=auto
blend-subtitles=video
sub-ass-override=no
embeddedfonts=yes
osc=yes
```

## 已知待办

- 已接入 `src/ui` 的媒体库/详情/播放页 UI 骨架（数据仍为 mock，未接 Emby API）
- 已接入 `src/emby` API 客户端静态库（尚未与 UI 数据层对接）
- 播放页的字幕/音轨选择仍为占位，未映射到 mpv track
- 还没做打包/自动更新
