#include "videohost.h"

#include <QDebug>
#include <QPalette>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStringList>
#include <QTimer>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#include <commctrl.h>

namespace {

constexpr UINT_PTR kMpvMouseSubclassId = 1;

struct ResizeChildrenContext
{
    int resized = 0;
    HWND host = nullptr;
    bool debug = false;
};

LRESULT CALLBACK mpvMouseSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                      UINT_PTR idSubclass, DWORD_PTR refData);

BOOL CALLBACK resizeChildWindow(HWND child, LPARAM param)
{
    auto *context = reinterpret_cast<ResizeChildrenContext *>(param);
    if (!context || !child || !context->host)
        return TRUE;

    // 只同步直接子窗口（mpv 的 VO 窗口）；更深的窗口由 mpv 自己维护，
    // 避免把 OSC/统计等内部子窗口误拉伸。
    if (::GetParent(child) != context->host)
        return TRUE;

    RECT parentRect{};
    if (!::GetClientRect(context->host, &parentRect))
        return TRUE;
    const int width = parentRect.right - parentRect.left;
    const int height = parentRect.bottom - parentRect.top;
    if (width <= 0 || height <= 0)
        return TRUE;

    RECT childRect{};
    if (!::GetClientRect(child, &childRect))
        return TRUE;
    if (childRect.right != width || childRect.bottom != height) {
        ::MoveWindow(child, 0, 0, width, height, TRUE);
        ++context->resized;
        if (context->debug) {
            qInfo() << "VideoHost: resize mpv child hwnd"
                    << reinterpret_cast<quintptr>(child) << "->" << width << "x" << height;
        }
    }
    return TRUE;
}

BOOL CALLBACK collectChildWindow(HWND child, LPARAM param)
{
    auto *lines = reinterpret_cast<QStringList *>(param);
    if (!lines || !child)
        return TRUE;

    wchar_t className[128]{};
    ::GetClassNameW(child, className, 128);
    RECT windowRect{};
    RECT clientRect{};
    ::GetWindowRect(child, &windowRect);
    ::GetClientRect(child, &clientRect);
    const HWND parent = ::GetParent(child);
    lines->append(QStringLiteral("  child hwnd=0x%1 class=%2 parent=0x%3 "
                                 "win=%4x%5 client=%6x%7 visible=%8")
                      .arg(reinterpret_cast<quintptr>(child), 0, 16)
                      .arg(QString::fromWCharArray(className))
                      .arg(reinterpret_cast<quintptr>(parent), 0, 16)
                      .arg(windowRect.right - windowRect.left)
                      .arg(windowRect.bottom - windowRect.top)
                      .arg(clientRect.right - clientRect.left)
                      .arg(clientRect.bottom - clientRect.top)
                      .arg(::IsWindowVisible(child) ? QStringLiteral("yes")
                                                    : QStringLiteral("no")));
    return TRUE;
}

LRESULT CALLBACK mpvMouseSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
                                      UINT_PTR idSubclass, DWORD_PTR refData)
{
    Q_UNUSED(idSubclass);
    auto *host = reinterpret_cast<VideoHost *>(refData);
    if (!host)
        return ::DefSubclassProc(hwnd, message, wParam, lParam);

    const QPoint position(static_cast<short>(LOWORD(lParam)),
                          static_cast<short>(HIWORD(lParam)));
    switch (message) {
    case WM_MOUSEMOVE:
        host->notifyMouseMove(position);
        break;
    case WM_LBUTTONDOWN:
        host->notifyMouseButton(0, position, false);
        break;
    case WM_LBUTTONDBLCLK:
        host->notifyMouseButton(0, position, true);
        break;
    case WM_RBUTTONDOWN:
        host->notifyMouseButton(2, position, false);
        break;
    case WM_MBUTTONDOWN:
        host->notifyMouseButton(1, position, false);
        break;
    case WM_LBUTTONUP:
        host->notifyMouseRelease(0);
        break;
    case WM_RBUTTONUP:
        host->notifyMouseRelease(2);
        break;
    case WM_MBUTTONUP:
        host->notifyMouseRelease(1);
        break;
    case WM_MOUSEWHEEL: {
        // WM_MOUSEWHEEL 的 lParam 是屏幕坐标，转成视频子窗口客户区坐标。
        POINT point{static_cast<short>(LOWORD(lParam)),
                    static_cast<short>(HIWORD(lParam))};
        ::ScreenToClient(hwnd, &point);
        const int delta = static_cast<short>(HIWORD(wParam));
        const int button = delta > 0 ? 3 : 4; // mpv: 3=WHEEL_UP, 4=WHEEL_DOWN
        const int steps = qMax(1, qAbs(delta) / WHEEL_DELTA);
        for (int i = 0; i < steps; ++i)
            host->notifyMouseWheel(button, QPoint(point.x, point.y));
        break;
    }
    case WM_NCDESTROY:
        ::RemoveWindowSubclass(hwnd, mpvMouseSubclassProc, kMpvMouseSubclassId);
        host->notifyChildDestroyed(reinterpret_cast<quintptr>(hwnd));
        break;
    default:
        break;
    }
    return ::DefSubclassProc(hwnd, message, wParam, lParam);
}

} // namespace
#endif

VideoHost::VideoHost(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setPalette(pal);

    m_syncTimer = new QTimer(this);
    m_syncTimer->setInterval(120);
    connect(m_syncTimer, &QTimer::timeout, this, &VideoHost::syncMpvChildGeometry);
    m_syncTimer->start();
}

VideoHost::~VideoHost()
{
#ifdef Q_OS_WIN
    if (m_mpvChildHandle) {
        ::RemoveWindowSubclass(reinterpret_cast<HWND>(m_mpvChildHandle),
                               mpvMouseSubclassProc, kMpvMouseSubclassId);
        m_mpvChildHandle = 0;
    }
#endif
}

quintptr VideoHost::nativeWindow()
{
    // Qt6 的 WId 在 Windows 上也是整型（quintptr），MSVC 不允许整型→整型的 reinterpret_cast；
    // 用 static_cast 保持跨平台（Qt5/HWND 指针场景不再支持，本项目固定 Qt6）。
    return static_cast<quintptr>(winId());
}

QSize VideoHost::minimumSizeHint() const
{
    return QSize(320, 180);
}

void VideoHost::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (qEnvironmentVariableIsSet("EMBY_PC_DEBUG_GEOMETRY"))
        qInfo() << "VideoHost: resize event" << event->size();
    syncMpvChildGeometry();
}

void VideoHost::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (qEnvironmentVariableIsSet("EMBY_PC_DEBUG_GEOMETRY"))
        qInfo() << "VideoHost: show event" << size();
    syncMpvChildGeometry();
}

void VideoHost::syncMpvChildGeometry()
{
#ifdef Q_OS_WIN
    const HWND host = reinterpret_cast<HWND>(winId());
    if (!host)
        return;

    RECT hostRect{};
    if (!::GetClientRect(host, &hostRect))
        return;
    const int width = hostRect.right - hostRect.left;
    const int height = hostRect.bottom - hostRect.top;
    if (width <= 0 || height <= 0)
        return;

    // mpv 会把它自己的窗口挂成我们的子窗口。EnumChildWindows 枚举所有子窗口，
    // 但只同步直接子窗口（mpv VO）；更深层窗口（如 OSC/统计）由 mpv 自己维护，
    // 避免被误拉伸。
    ResizeChildrenContext context;
    context.host = host;
    context.debug = qEnvironmentVariableIsSet("EMBY_PC_DEBUG_GEOMETRY");
    ::EnumChildWindows(host, resizeChildWindow, reinterpret_cast<LPARAM>(&context));

    if (context.debug && context.resized > 0) {
        qInfo() << "VideoHost: synced" << context.resized << "mpv child window(s) to"
                << width << "x" << height;
    }

    // 输入转发：--wid 模式下 mpv 禁用自己窗口的输入（mpv issue #4795），
    // 宿主必须把 mpv 子 HWND 的鼠标消息转发成 mpv 的 `mouse` 命令。
    if (m_mpvChildHandle && !::IsWindow(reinterpret_cast<HWND>(m_mpvChildHandle)))
        m_mpvChildHandle = 0;
    if (HWND child = ::GetWindow(host, GW_CHILD))
        installMouseSubclass(reinterpret_cast<quintptr>(child));
#endif
}

void VideoHost::installMouseSubclass(quintptr child)
{
#ifdef Q_OS_WIN
    if (!child)
        return;
    if (m_mpvChildHandle == child)
        return;
    if (m_mpvChildHandle) {
        ::RemoveWindowSubclass(reinterpret_cast<HWND>(m_mpvChildHandle),
                               mpvMouseSubclassProc, kMpvMouseSubclassId);
        m_mpvChildHandle = 0;
    }
    m_mpvChildHandle = child;
    if (!::SetWindowSubclass(reinterpret_cast<HWND>(child), mpvMouseSubclassProc,
                             kMpvMouseSubclassId, reinterpret_cast<DWORD_PTR>(this))) {
        qWarning() << "VideoHost: SetWindowSubclass failed for mpv child"
                   << child << "error" << ::GetLastError();
        m_mpvChildHandle = 0;
        return;
    }
    qInfo() << "VideoHost: mouse subclass installed on mpv child" << child;
#else
    Q_UNUSED(child);
#endif
}

void VideoHost::notifyMouseMove(const QPoint &position)
{
    ++m_mouseEventCount;
    if (qEnvironmentVariableIntValue("EMBY_PC_MPV_OSC") > 0
        && (m_mouseEventCount == 1 || m_mouseEventCount % 20 == 0)) {
        qInfo() << "VideoHost: mouse move forwarded" << position
                << "count" << m_mouseEventCount;
    }
    emit mouseMoved(position);
}

void VideoHost::notifyMouseButton(int button, const QPoint &position, bool doubleClick)
{
    ++m_mouseEventCount;
    if (qEnvironmentVariableIntValue("EMBY_PC_MPV_OSC") > 0) {
        qInfo() << "VideoHost: mouse button forwarded" << button << position
                << "double" << doubleClick << "count" << m_mouseEventCount;
    }
    emit mouseButtonPressed(button, position, doubleClick);
}

void VideoHost::notifyMouseRelease(int button)
{
    ++m_mouseEventCount;
    if (qEnvironmentVariableIntValue("EMBY_PC_MPV_OSC") > 0)
        qInfo() << "VideoHost: mouse release forwarded" << button;
    emit mouseButtonReleased(button);
}

void VideoHost::notifyMouseWheel(int button, const QPoint &position)
{
    ++m_mouseEventCount;
    if (qEnvironmentVariableIntValue("EMBY_PC_MPV_OSC") > 0) {
        qInfo() << "VideoHost: mouse wheel forwarded" << button << position
                << "count" << m_mouseEventCount;
    }
    emit mouseWheel(button, position);
}

void VideoHost::notifyChildDestroyed(quintptr child)
{
    if (m_mpvChildHandle == child)
        m_mpvChildHandle = 0;
}

QString VideoHost::diagnosticSummary()
{
    QStringList lines;
    lines.append(QStringLiteral("VideoHost: geometry=%1x%2 visible=%3 nativeWindow=0x%4")
                     .arg(width())
                     .arg(height())
                     .arg(isVisible() ? QStringLiteral("yes") : QStringLiteral("no"))
                     .arg(static_cast<quintptr>(winId()), 0, 16));

#ifdef Q_OS_WIN
    const HWND host = reinterpret_cast<HWND>(winId());
    if (!host) {
        lines.append(QStringLiteral("  host HWND: <null>"));
        return lines.join(QLatin1Char('\n'));
    }

    RECT clientRect{};
    ::GetClientRect(host, &clientRect);
    RECT windowRect{};
    ::GetWindowRect(host, &windowRect);
    wchar_t hostClassName[128]{};
    ::GetClassNameW(host, hostClassName, 128);
    lines.append(QStringLiteral("  host HWND=0x%1 class=%2 client=%3x%4 window=%5x%6")
                     .arg(reinterpret_cast<quintptr>(host), 0, 16)
                     .arg(QString::fromWCharArray(hostClassName))
                     .arg(clientRect.right - clientRect.left)
                     .arg(clientRect.bottom - clientRect.top)
                     .arg(windowRect.right - windowRect.left)
                     .arg(windowRect.bottom - windowRect.top));
    lines.append(QStringLiteral("  mouse subclass hwnd=0x%1 forwarded_events=%2")
                     .arg(m_mpvChildHandle, 0, 16)
                     .arg(m_mouseEventCount));
    lines.append(QStringLiteral("  mpv child windows:"));
    ::EnumChildWindows(host, collectChildWindow, reinterpret_cast<LPARAM>(&lines));
#else
    lines.append(QStringLiteral("  child windows: only available on Windows"));
#endif
    return lines.join(QLatin1Char('\n'));
}
