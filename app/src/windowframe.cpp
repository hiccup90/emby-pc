#include "windowframe.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QDebug>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>
#include <QWidget>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <dwmapi.h>
#include <shellapi.h>

#ifndef HTMAXBUTTON
#define HTMAXBUTTON 9
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_COLOR_NONE
#define DWMWA_COLOR_NONE 0xFFFFFFFE
#endif
// Win11 DWM 圆角偏好；Win10 会忽略/返回错误，不影响窗口。
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMCP_DONOTROUND
#define DWMCP_DONOTROUND 1
#endif

namespace {

constexpr int kDefaultTitleBarHeight = 36;
constexpr int kMaximizeButtonWidth = 46;
constexpr int kAutoHideTaskBarThickness = 2;

QRect rectToQ(const RECT &rect)
{
    return QRect(rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
}

int windowDpi(HWND hwnd)
{
    // GetDpiForWindow 在 Win10 1607+ 可用；Qt 6 目标平台满足。
    const int dpi = static_cast<int>(::GetDpiForWindow(hwnd));
    return dpi > 0 ? dpi : 96;
}

QSize scaledSize(const QSize &logical, int dpi)
{
    const qreal scale = dpi / 96.0;
    return QSize(qRound(logical.width() * scale), qRound(logical.height() * scale));
}

bool isMaxButtonChild(QWidget *host, const QPoint &globalPosition)
{
    if (!host)
        return false;
    QWidget *child = host->childAt(host->mapFromGlobal(globalPosition));
    for (QWidget *widget = child; widget && widget != host; widget = widget->parentWidget()) {
        if (qobject_cast<QAbstractButton *>(widget))
            return true;
        if (widget->objectName().contains(QStringLiteral("Button"), Qt::CaseInsensitive))
            return true;
    }
    return false;
}

} // namespace
#endif // Q_OS_WIN

WindowFrame::WindowFrame(QWidget *window, QObject *parent)
    : QObject(parent)
    , m_window(window)
{
    if (!m_window)
        return;

    // Qt 在 Windows 上遇到 FramelessWindowHint 会设 WS_POPUP；先保留/补回原生
    // thick frame/caption，再由 WM_NCCALCSIZE 去掉标题栏，DWM 阴影和缩放边才在。
    Qt::WindowFlags flags = m_window->windowFlags();
    flags |= Qt::FramelessWindowHint;
    flags |= Qt::WindowMinMaxButtonsHint;
    m_window->setWindowFlags(flags);
    m_window->setAttribute(Qt::WA_NativeWindow, true);

    if (m_window->minimumWidth() > 800 || m_window->minimumHeight() > 480)
        m_window->setMinimumSize(800, 480); // WIN-01 下限；T27 可再按页面调整

#ifdef Q_OS_WIN
    m_hwnd = static_cast<quintptr>(m_window->winId());
    m_dpi = windowDpi(reinterpret_cast<HWND>(m_hwnd));
    installNativeStyles();
    applyDwmFrame();
    m_wasMaximized = ::IsZoomed(reinterpret_cast<HWND>(m_hwnd)) != FALSE;
    if (QCoreApplication *app = QCoreApplication::instance())
        app->installNativeEventFilter(this);
#else
    Q_UNUSED(m_hwnd);
#endif
}

WindowFrame::~WindowFrame()
{
    if (QCoreApplication *app = QCoreApplication::instance())
        app->removeNativeEventFilter(this);
}

WindowFrame *WindowFrame::attach(QWidget *window)
{
    if (!window || !window->isWindow())
        return nullptr;
    if (auto *existing = window->findChild<WindowFrame *>(QString(), Qt::FindDirectChildrenOnly))
        return existing;
    return new WindowFrame(window, window);
}

void WindowFrame::setTitleBarHeight(int logicalPixels)
{
    m_titleBarHeight = qMax(0, logicalPixels);
}

QWidget *WindowFrame::titleBarHost() const
{
    return m_titleBarWidget ? m_titleBarWidget.data() : m_window;
}

void WindowFrame::setTitleBarWidget(QWidget *titleBar)
{
    m_titleBarWidget = titleBar;
}

void WindowFrame::setMaximizeButtonRect(const QRect &logicalRect)
{
    m_maximizeButtonRect = logicalRect;
}

bool WindowFrame::isMaximized() const
{
#ifdef Q_OS_WIN
    if (!m_hwnd)
        return false;
    return ::IsZoomed(reinterpret_cast<HWND>(m_hwnd)) != FALSE;
#else
    return m_window ? m_window->isMaximized() : false;
#endif
}

void WindowFrame::setMinimumWindowSize(const QSize &size)
{
    m_minimumSize = QSize(qMax(1, size.width()), qMax(1, size.height()));
    qInfo() << "WindowFrame: minimum window size" << m_minimumSize;
}

void WindowFrame::toggleMaximize()
{
    if (!m_window)
        return;
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(m_hwnd);
    if (!hwnd)
        return;
    ::ShowWindow(hwnd, ::IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
#else
    if (m_window->isMaximized())
        m_window->showNormal();
    else
        m_window->showMaximized();
    const bool maximized = m_window->isMaximized();
    if (maximized != m_wasMaximized) {
        m_wasMaximized = maximized;
        emit maximizedChanged(maximized);
    }
#endif
}

void WindowFrame::beginSystemMove()
{
    if (!m_window)
        return;
    if (QWindow *handle = m_window->windowHandle()) {
        if (handle->startSystemMove())
            return;
    }
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(m_hwnd);
    if (hwnd) {
        ::ReleaseCapture();
        ::SendMessage(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
    }
#endif
}

void WindowFrame::beginSystemResize(Qt::Edges edges)
{
    if (!m_window)
        return;
    if (QWindow *handle = m_window->windowHandle())
        handle->startSystemResize(edges);
}

Qt::Edges WindowFrame::edgeHitTestForTest(const QSize &clientSize, const QPoint &point,
                                          int dpi, bool maximized)
{
    if (maximized || clientSize.isEmpty())
        return {};
    const qreal scale = qMax(1, dpi) / 96.0;
    const int border = qMax(6, qRound(8.0 * scale));
    Qt::Edges edges;
    if (point.x() < border)
        edges |= Qt::LeftEdge;
    if (point.x() >= clientSize.width() - border)
        edges |= Qt::RightEdge;
    if (point.y() < border)
        edges |= Qt::TopEdge;
    if (point.y() >= clientSize.height() - border)
        edges |= Qt::BottomEdge;
    return edges;
}

#ifdef Q_OS_WIN

void WindowFrame::installNativeStyles()
{
    if (!m_hwnd)
        return;
    applyWindowCornerPreference();
    HWND hwnd = reinterpret_cast<HWND>(m_hwnd);
    const LONG_PTR required = WS_CAPTION | WS_SYSMENU | WS_THICKFRAME
                              | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    LONG_PTR style = ::GetWindowLongPtr(hwnd, GWL_STYLE);
    if ((style & required) == required && !(style & WS_POPUP))
        return;

    style &= ~static_cast<LONG_PTR>(WS_POPUP);
    style |= required;
    ::SetWindowLongPtr(hwnd, GWL_STYLE, style);
    ::SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
                       | SWP_FRAMECHANGED);
    qInfo() << "WindowFrame: native styles applied"
            << Qt::hex << static_cast<qulonglong>(style);
}

void WindowFrame::applyWindowCornerPreference()
{
    if (!m_hwnd)
        return;
    HWND hwnd = reinterpret_cast<HWND>(m_hwnd);
    const int preference = DWMCP_DONOTROUND;
    const HRESULT hr = ::DwmSetWindowAttribute(
        hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
    qInfo() << "WindowFrame: DwmSetWindowAttribute(DWMWA_WINDOW_CORNER_PREFERENCE)"
            << "DWMCP_DONOTROUND"
            << "hr=" << Qt::hex << static_cast<qulonglong>(hr) << Qt::dec
            << "succeeded=" << SUCCEEDED(hr);
}

void WindowFrame::applyDwmFrame()
{
    if (!m_hwnd)
        return;
    applyWindowCornerPreference();
    HWND hwnd = reinterpret_cast<HWND>(m_hwnd);
    // 1px 仅用于 DWM 外框/阴影，不是 resize 热区；避免可见黑边/桌面透出。
    MARGINS margins{1, 1, 1, 1};
    const HRESULT hr = ::DwmExtendFrameIntoClientArea(hwnd, &margins);
    if (FAILED(hr))
        qWarning() << "WindowFrame: DwmExtendFrameIntoClientArea failed" << Qt::hex
                   << static_cast<qulonglong>(hr);

    // Win11：不要额外画彩色边框；保留 DWM 默认阴影/圆角。
    const DWORD borderColor = DWMWA_COLOR_NONE;
    ::DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &borderColor,
                            sizeof(borderColor));
}

void WindowFrame::updateDpi()
{
    if (!m_hwnd)
        return;
    m_dpi = windowDpi(reinterpret_cast<HWND>(m_hwnd));
}

QRect WindowFrame::maximizedWorkArea() const
{
    if (!m_hwnd)
        return {};
    HWND hwnd = reinterpret_cast<HWND>(m_hwnd);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    const HMONITOR monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!monitor || !::GetMonitorInfo(monitor, &monitorInfo))
        return {};

    RECT work = monitorInfo.rcWork;
    // 自动隐藏任务栏：Windows 的 rcWork 可能已等于整屏，补 2px 内缩防止任务栏
    // 弹起时压住内容；固定任务栏则直接使用 rcWork。
    APPBARDATA appBar{};
    appBar.cbSize = sizeof(appBar);
    if (::SHAppBarMessage(ABM_GETSTATE, &appBar) & ABS_AUTOHIDE) {
        APPBARDATA taskbar{};
        taskbar.cbSize = sizeof(taskbar);
        if (::SHAppBarMessage(ABM_GETTASKBARPOS, &taskbar)) {
            switch (taskbar.uEdge) {
            case ABE_LEFT:
                work.left = monitorInfo.rcMonitor.left + kAutoHideTaskBarThickness;
                break;
            case ABE_TOP:
                work.top = monitorInfo.rcMonitor.top + kAutoHideTaskBarThickness;
                break;
            case ABE_RIGHT:
                work.right = monitorInfo.rcMonitor.right - kAutoHideTaskBarThickness;
                break;
            case ABE_BOTTOM:
                work.bottom = monitorInfo.rcMonitor.bottom - kAutoHideTaskBarThickness;
                break;
            default:
                break;
            }
        }
    }
    return rectToQ(work);
}

bool WindowFrame::handleNativeMessage(void *message, qintptr *result)
{
    auto *msg = static_cast<MSG *>(message);
    if (!msg || msg->hwnd != reinterpret_cast<HWND>(m_hwnd))
        return false;

    HWND hwnd = msg->hwnd;
    switch (msg->message) {
    case WM_NCCALCSIZE: {
        if (!msg->wParam)
            return false;
        auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam);
        if (!params)
            return false;

        RECT client = params->rgrc[0];
        if (::IsZoomed(hwnd)) {
            const QRect work = maximizedWorkArea();
            if (work.isValid()) {
                client.left = work.left();
                client.top = work.top();
                client.right = work.left() + work.width();
                client.bottom = work.top() + work.height();
            } else {
                ::GetWindowRect(hwnd, &client);
            }
        } else {
            // 非最大化：客户区就是整个窗口矩形（DWM 外框由 DwmExtendFrame 绘制）。
            ::GetWindowRect(hwnd, &client);
        }
        params->rgrc[0] = client;
        *result = 0;
        return true;
    }

    case WM_GETMINMAXINFO: {
        auto *info = reinterpret_cast<MINMAXINFO *>(msg->lParam);
        if (!info)
            return false;
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        const HMONITOR monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (monitor && ::GetMonitorInfo(monitor, &monitorInfo)) {
            info->ptMaxPosition.x = monitorInfo.rcWork.left;
            info->ptMaxPosition.y = monitorInfo.rcWork.top;
            info->ptMaxSize.x = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
            info->ptMaxSize.y = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
        }
        const int dpi = windowDpi(hwnd);
        // T52：只用显式设置的窗口最小尺寸，避免 Qt widget minimumSize 与播放模式冲突。
        const QSize minSize = m_minimumSize;
        const QSize physicalMin = scaledSize(minSize, dpi);
        info->ptMinTrackSize.x = physicalMin.width();
        info->ptMinTrackSize.y = physicalMin.height();
        *result = 0;
        return true;
    }

    case WM_NCHITTEST: {
        const int screenX = static_cast<short>(LOWORD(msg->lParam));
        const int screenY = static_cast<short>(HIWORD(msg->lParam));
        POINT point{screenX, screenY};
        ::ScreenToClient(hwnd, &point);
        RECT client{};
        if (!::GetClientRect(hwnd, &client))
            return false;
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        const int dpi = windowDpi(hwnd);
        const qreal scale = dpi / 96.0;
        const int border = qMax(6, qRound(8.0 * scale));
        const bool maximized = ::IsZoomed(hwnd) != FALSE;
        if (qEnvironmentVariableIsSet("EMBY_PC_DEBUG_HITTEST")
            || qEnvironmentVariableIsSet("EMBY_PC_DEBUG_GEOMETRY")) {
            qInfo() << "WindowFrame: WM_NCHITTEST point" << QPoint(point.x, point.y)
                    << "client" << QSize(width, height) << "state"
                    << int(m_window->windowState()) << "maximized" << maximized
                    << "fullscreen" << m_window->isFullScreen() << "border" << border;
        }

        const Qt::Edges edgeHit = edgeHitTestForTest(QSize(width, height), QPoint(point.x, point.y),
                                                       dpi, maximized);
        if (edgeHit) {
            const bool left = edgeHit.testFlag(Qt::LeftEdge);
            const bool right = edgeHit.testFlag(Qt::RightEdge);
            const bool top = edgeHit.testFlag(Qt::TopEdge);
            const bool bottom = edgeHit.testFlag(Qt::BottomEdge);
            if (left && top) { *result = HTTOPLEFT; return true; }
            if (right && top) { *result = HTTOPRIGHT; return true; }
            if (left && bottom) { *result = HTBOTTOMLEFT; return true; }
            if (right && bottom) { *result = HTBOTTOMRIGHT; return true; }
            if (left) { *result = HTLEFT; return true; }
            if (right) { *result = HTRIGHT; return true; }
            if (top) { *result = HTTOP; return true; }
            if (bottom) { *result = HTBOTTOM; return true; }
        }

        const int titleHeight = qMax(0, qRound(m_titleBarHeight * scale));
        if (point.y >= 0 && point.y < titleHeight && point.x >= 0 && point.x < width) {
            QRect maxRect; // logical window coordinates before DPR scaling
            if (m_maximizeButtonRect.isValid()) {
                const QPoint topLeft = m_titleBarWidget
                                           ? m_titleBarWidget->mapTo(m_window,
                                                                     m_maximizeButtonRect.topLeft())
                                           : m_maximizeButtonRect.topLeft();
                maxRect = QRect(qRound(topLeft.x() * scale), qRound(topLeft.y() * scale),
                                qRound(m_maximizeButtonRect.width() * scale),
                                qRound(m_maximizeButtonRect.height() * scale));
            } else if (m_titleBarWidget) {
                const QPoint topLeft = m_titleBarWidget->mapTo(
                    m_window, QPoint(qMax(0, m_titleBarWidget->width() - kMaximizeButtonWidth), 0));
                maxRect = QRect(qRound(topLeft.x() * scale), qRound(topLeft.y() * scale),
                                qRound(kMaximizeButtonWidth * scale), qRound(32 * scale));
            } else {
                const int buttonWidth = qRound(kMaximizeButtonWidth * scale);
                maxRect = QRect(width - buttonWidth, 0, buttonWidth, qRound(32 * scale));
            }
            if (maxRect.contains(point.x, point.y)) {
                *result = HTMAXBUTTON;
                return true;
            }

            // 自绘按钮必须收到 Qt 鼠标事件；标题栏空白才交给系统 HTCAPTION。
            const QPoint globalPoint(screenX, screenY);
            if (isMaxButtonChild(m_titleBarWidget ? m_titleBarWidget.data() : m_window,
                                 globalPoint)) {
                *result = HTCLIENT;
                return true;
            }
            *result = HTCAPTION;
            return true;
        }
        *result = HTCLIENT;
        return true;
    }

    case WM_DPICHANGED:
        // 只更新本地 DPI 供下一次命中测试使用；让 Qt 继续处理 WM_DPICHANGED，
        // 否则 Qt 的 backing store/屏幕 DPR 可能不更新，导致文字/视频模糊。
        updateDpi();
        qInfo() << "WindowFrame: DPI changed" << m_dpi;
        return false;

    case WM_ACTIVATE:
        installNativeStyles();
        applyDwmFrame();
        break;

    case WM_SIZE: {
        const bool maximized = ::IsZoomed(hwnd) != FALSE;
        if (maximized != m_wasMaximized) {
            m_wasMaximized = maximized;
            emit maximizedChanged(maximized);
        }
        break;
    }

    case WM_SYSCOMMAND:
        // SC_MINIMIZE / SC_MAXIMIZE / SC_RESTORE 交给 DefWindowProc；WM_SIZE 负责发信号。
        break;
    }
    return false;
}

#endif // Q_OS_WIN

bool WindowFrame::nativeEventFilter(const QByteArray &eventType, void *message,
                                    qintptr *result)
{
#ifdef Q_OS_WIN
    if (eventType != "windows_generic_MSG" && eventType != "windows_dispatcher_MSG")
        return false;
    return handleNativeMessage(message, result);
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
    Q_UNUSED(result);
    return false;
#endif
}
