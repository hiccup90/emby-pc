#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QSize>

class QWidget;

// M3-1 / T26：Windows DWM 原生无边框窗口层。
//
// 用法（必须在 show() 前 attach）：
//   auto *frame = WindowFrame::attach(window);
//   frame->setTitleBarHeight(38);
//   frame->setTitleBarWidget(titleBar);       // 可选；默认 titleBarHost() 返回顶层窗口
//   frame->setMaximizeButtonRect(maxButtonRect); // 可选；精确 HTMAXBUTTON/Snap
//
// Windows 上负责：FramelessWindowHint + 保留 WS_THICKFRAME/WS_CAPTION + DWM 扩展框、
// WM_NCCALCSIZE / WM_NCHITTEST / WM_GETMINMAXINFO / WM_DPICHANGED / WM_SYSCOMMAND。
// Linux/其他平台只提供同样的 QWidget 层 API，保证 offscreen 编译与 T27 测试可用。
class WindowFrame : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
public:
    explicit WindowFrame(QWidget *window, QObject *parent = nullptr);
    ~WindowFrame() override;

    // 顶层窗口 attach；重复调用返回已有实例。窗口必须 isWindow()。
    static WindowFrame *attach(QWidget *window);

    QWidget *window() const { return m_window; }

    // 逻辑像素；默认 36。Windows 命中测试会按 devicePixelRatio 转物理像素。
    void setTitleBarHeight(int logicalPixels);
    int titleBarHeight() const { return m_titleBarHeight; }

    // 默认返回顶层窗口；若设置了 setTitleBarWidget，则返回该 TitleBar widget。
    QWidget *titleBarHost() const;
    void setTitleBarWidget(QWidget *titleBar); // 可选：T27 用

    // 可选：最大化按钮的逻辑坐标；设置了 titleBarWidget 时相对 TitleBar 本地坐标，
    // 否则相对顶层窗口客户区。无效时按 TitleBar（或窗口）最右侧 46x32 默认区域。
    void setMaximizeButtonRect(const QRect &logicalRect);
    QRect maximizeButtonRect() const { return m_maximizeButtonRect; }

    bool isMaximized() const;
    void toggleMaximize();
    void beginSystemMove();
    void beginSystemResize(Qt::Edges edges);

    // T46 测试：与 WM_NCHITTEST 相同的边缘判定；最大化/全屏返回空。
    static Qt::Edges edgeHitTestForTest(const QSize &clientSize, const QPoint &point,
                                        int dpi, bool maximized);

signals:
    void maximizedChanged(bool maximized);

protected:
    bool nativeEventFilter(const QByteArray &eventType, void *message,
                           qintptr *result) override;

private:
#ifdef Q_OS_WIN
    void installNativeStyles();
    void applyDwmFrame();
    void updateDpi();
    QRect maximizedWorkArea() const;
    bool handleNativeMessage(void *message, qintptr *result);
#endif

    QWidget *m_window = nullptr;
    QPointer<QWidget> m_titleBarWidget;
    quintptr m_hwnd = 0;
    int m_titleBarHeight = 36;
    QRect m_maximizeButtonRect;
    QSize m_minimumSize = QSize(800, 480);
    bool m_wasMaximized = false;
    int m_dpi = 96;
};
