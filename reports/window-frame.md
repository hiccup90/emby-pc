# M3-1 / T26+T27 DWM 无边框窗口与自绘标题栏

> 任务：task-26 · 负责人：build-verifier · 日期：2026-11-05
> 范围：app/src/windowframe.h/.cpp（新）、app/CMakeLists.txt、本报告。未引入 QWindowKit，未修改 app/src/ui/（标题栏 UI 由 T27 做）。

## 1. API 契约（T27）

```cpp
class WindowFrame : public QObject {
    Q_OBJECT
public:
    static WindowFrame *attach(QWidget *window); // 顶层窗口，show() 前调用
    void setTitleBarHeight(int logicalPixels);   // 默认 36
    QWidget *titleBarHost() const;               // setTitleBarWidget 后返回 TitleBar，否则返回顶层窗口
    void setTitleBarWidget(QWidget *titleBar);   // T27 TitleBar 直接子控件
    void setMaximizeButtonRect(const QRect &logicalRect); // TitleBar 本地逻辑坐标
    bool isMaximized() const;
    void toggleMaximize();
    void beginSystemMove();
    void beginSystemResize(Qt::Edges edges);

signals:
    void maximizedChanged(bool maximized);
};
```

T27 集成约定：TitleBar 是 MainWindow 的直接子控件，不包含 QMenuBar；最大化按钮矩形传 TitleBar 本地逻辑坐标；最小化/关闭按钮通过 childAt(QAbstractButton) 返回 HTCLIENT 获得 Qt click；最大化按钮区域返回 HTMAXBUTTON 以启用 Win11 Snap Layouts。

## 2. Windows 原生实现

### 2.1 窗口样式与 DWM

- 构造时设置 `Qt::FramelessWindowHint | Qt::WindowMinMaxButtonsHint`，保留 `WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX`（去掉 Qt Frameless 产生的 `WS_POPUP`）。
- `DwmExtendFrameIntoClientArea({1,1,1,1})` 保留 DWM 阴影/外框；Win11 设 `DWMWA_BORDER_COLOR = DWMWA_COLOR_NONE`，避免彩色边。
- 不设置 `WA_TranslucentBackground`，避免黑边/桌面透出。

### 2.2 消息处理

- **WM_NCCALCSIZE**：wParam=TRUE 时客户区 = 整个窗口矩形；最大化时用 `MonitorFromWindow + GetMonitorInfo().rcWork` 夹紧到工作区，避免最大化溢出/1px 缝。任务栏自动隐藏时按任务栏所在边内缩 2px。
- **WM_GETMINMAXINFO**：`ptMaxPosition/ptMaxSize = rcWork`；`ptMinTrackSize = max(window.minimumSize, 800x480) × dpi/96`。
- **WM_NCHITTEST**：
  - 非最大化四边/四角 8px（最小 6px，按 DPI 缩放）→ HTLEFT/HTRIGHT/HTTOP/HTBOTTOM/四角；
  - TitleBar 顶部 `titleBarHeight` 逻辑像素：最大化按钮矩形 → **HTMAXBUTTON**（Snap Layouts）；TitleBar 内 QAbstractButton（最小化/关闭等）→ **HTCLIENT**（Qt 能收到 click）；其余空白 → **HTCAPTION**（拖动/双击最大化由系统处理）；
  - 菜单栏及其他区域 → HTCLIENT。
- **WM_DPICHANGED**：按 lParam 的 suggested rect `SetWindowPos`，更新内部 DPI；`WM_DPICHANGED` 后命中边框/标题栏按新 DPI 重算。
- **WM_SYSCOMMAND**：SC_MINIMIZE/SC_MAXIMIZE/SC_RESTORE 交给 DefWindowProc；**WM_SIZE** 检测 `IsZoomed` 变化并 emit `maximizedChanged`。
- **WM_ACTIVATE**：重新确认原生样式并 `DwmExtendFrameIntoClientArea`，保证最大化/还原后 DWM frame 一致。

### 2.3 多显示器/最小尺寸

- 最大化/还原、`WM_GETMINMAXINFO` 均以 `MonitorFromWindow(MONITOR_DEFAULTTONEAREST)` 当前显示器工作区计算。
- 最小尺寸默认 800x480；T27 可再按页面调整（播放页 320x180 由上层布局保证）。
- 窗口位置恢复由上层负责（T27 WIN-04），本层不抢 normalGeometry。

## 3. Linux 编译隔离

- 所有 Win32 头文件/API 包在 `#ifdef Q_OS_WIN`；非 Windows 仅提供 QWidget 层 API（attach/titleBarHost/setTitleBarHeight/isMaximized/toggleMaximize/maximizedChanged），保证 offscreen 编译与 T27 测试可用。
- 本机 Linux Debug 构建通过；app 可执行文件 OK。

## 4. 自测

### 4.1 offscreen API 测试（临时程序，不入库）

```text
attach true
hostIsTitleBar true
maxBefore false
maxChanged true
maxAfter true changed true
exit=0
```

覆盖：attach 幂等、titleBarHost 返回 TitleBar、setMaximizeButtonRect、toggleMaximize/maximizedChanged、beginSystemMove 不崩溃。

### 4.2 静态检查

- `grep QWindowKit`：app/src/windowframe.* 无 QWindowKit 引用。
- Linux `-Wall -Wextra` 构建无新增 warning。
- Windows 原生行为需实机验收（见 5）。

## 5. Windows 实机验收步骤

1. 用新包启动，确认系统标题栏消失，但窗口仍有 DWM 阴影、四边/四角可拖动缩放。
2. 拖到屏幕上/下/左/右边缘并最大化：截图检查最外 1px 无黑边、无桌面透出；最大化客户区与工作区误差 ±1px。
3. 鼠标悬停最大化按钮：出现 Win11 Snap Layouts；双击 TitleBar 空白：最大化/还原；最小化/关闭按钮可点击。
4. 100%/125%/150%/200% 缩放、双屏不同 DPI：标题栏/按钮不重叠，视频铺满，跨屏不黑屏。
5. 将窗口缩到 800x480（播放页可到 320x180 区域）确认控件可用；确认代码无 900x600/960x540 硬编码下限。
6. 进入播放模式/全屏：TitleBar 由 T27 隐藏，视频区域不受 DWM frame 影响，退出恢复。
7. app.log 应出现 `WindowFrame: native styles applied`、`DPI changed`；无异常 `DwmExtendFrameIntoClientArea failed`。
