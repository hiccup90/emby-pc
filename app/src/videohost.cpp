#include "videohost.h"

#include <QDebug>
#include <QMouseEvent>
#include <QPalette>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStringList>
#include <QTimer>
#include <QWheelEvent>

#ifdef Q_OS_WIN
#include <qt_windows.h>

namespace {

struct ResizeChildrenContext
{
    int resized = 0;
    HWND host = nullptr;
    bool debug = false;
};

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

} // namespace
#endif

VideoHost::VideoHost(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // T22：mpv 子窗口被设为 WS_EX_TRANSPARENT，鼠标事件会落到本控件。
    // 必须开启 mouseTracking，暂停时鼠标移动才能继续唤醒/驱动控制条。
    setMouseTracking(true);

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
    m_passthroughChild = 0;
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

QPoint VideoHost::toMpvPosition(const QPointF &position) const
{
    // mpv 的 mouse 命令使用视频窗口物理像素；Qt 坐标是逻辑像素，需乘 DPR。
    const qreal scale = devicePixelRatioF();
    return QPoint(qRound(position.x() * scale), qRound(position.y() * scale));
}

void VideoHost::mouseMoveEvent(QMouseEvent *event)
{
    ++m_mouseEventCount;
    const QPoint position = toMpvPosition(event->position());
    if (qEnvironmentVariableIsSet("EMBY_PC_DEBUG_INPUT")
        || (qEnvironmentVariableIntValue("EMBY_PC_MPV_OSC") > 0
            && (m_mouseEventCount == 1 || m_mouseEventCount % 20 == 0))) {
        qInfo() << "VideoHost: mouse move forwarded" << position
                << "count" << m_mouseEventCount;
    }
    emit mouseMoved(position);
    event->accept();
}

void VideoHost::mousePressEvent(QMouseEvent *event)
{
    ++m_mouseEventCount;
    // 点击视频后把焦点还给 Qt 主窗口，键盘继续走 Qt 的 eventFilter/快捷键。
    if (QWidget *top = window()) {
        top->activateWindow();
        top->setFocus(Qt::MouseFocusReason);
    }

    int button = -1;
    switch (event->button()) {
    case Qt::LeftButton:
        button = 0;
        break;
    case Qt::MiddleButton:
        button = 1;
        break;
    case Qt::RightButton:
        button = 2;
        break;
    default:
        break;
    }
    if (button < 0) {
        event->ignore();
        return;
    }

    const QPoint position = toMpvPosition(event->position());
    if (qEnvironmentVariableIsSet("EMBY_PC_DEBUG_INPUT"))
        qInfo() << "VideoHost: mouse button forwarded" << button << position
                << "count" << m_mouseEventCount;
    emit mouseButtonPressed(button, position, false);
    event->accept();
}

void VideoHost::mouseDoubleClickEvent(QMouseEvent *event)
{
    ++m_mouseEventCount;
    if (QWidget *top = window()) {
        top->activateWindow();
        top->setFocus(Qt::MouseFocusReason);
    }

    int button = -1;
    switch (event->button()) {
    case Qt::LeftButton:
        button = 0;
        break;
    case Qt::MiddleButton:
        button = 1;
        break;
    case Qt::RightButton:
        button = 2;
        break;
    default:
        break;
    }
    if (button < 0) {
        event->ignore();
        return;
    }

    const QPoint position = toMpvPosition(event->position());
    if (qEnvironmentVariableIsSet("EMBY_PC_DEBUG_INPUT"))
        qInfo() << "VideoHost: mouse double click forwarded" << button << position
                << "count" << m_mouseEventCount;
    emit mouseButtonPressed(button, position, true);
    event->accept();
}

void VideoHost::mouseReleaseEvent(QMouseEvent *event)
{
    ++m_mouseEventCount;
    int button = -1;
    switch (event->button()) {
    case Qt::LeftButton:
        button = 0;
        break;
    case Qt::MiddleButton:
        button = 1;
        break;
    case Qt::RightButton:
        button = 2;
        break;
    default:
        break;
    }
    if (button >= 0)
        emit mouseButtonReleased(button);
    event->accept();
}

void VideoHost::wheelEvent(QWheelEvent *event)
{
    ++m_mouseEventCount;
    int delta = event->angleDelta().y();
    if (delta == 0)
        delta = event->pixelDelta().y();
    if (delta == 0) {
        event->ignore();
        return;
    }

    const int button = delta > 0 ? 3 : 4; // mpv: 3=WHEEL_UP, 4=WHEEL_DOWN
    const int steps = qMax(1, qAbs(delta) / 120);
    const QPoint position = toMpvPosition(event->position());
    if (qEnvironmentVariableIsSet("EMBY_PC_DEBUG_INPUT"))
        qInfo() << "VideoHost: mouse wheel forwarded" << button << position
                << "steps" << steps << "count" << m_mouseEventCount;
    for (int i = 0; i < steps; ++i)
        emit mouseWheel(button, position);
    event->accept();
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

    // T22：不再跨线程 SetWindowSubclass。给直接子窗口加 WS_EX_TRANSPARENT，
    // 鼠标消息穿透到 Qt 的 VideoHost；WS_EX_NOACTIVATE 防止点击激活 mpv 子窗口。
    if (HWND child = ::GetWindow(host, GW_CHILD)) {
        ensureMousePassthrough(reinterpret_cast<quintptr>(child));
    } else {
        m_passthroughChild = 0;
    }
#endif
}

#ifdef Q_OS_WIN
void VideoHost::ensureMousePassthrough(quintptr child)
{
    if (!child)
        return;
    HWND hwnd = reinterpret_cast<HWND>(child);
    const LONG_PTR required = WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    const LONG_PTR current = ::GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if ((current & required) == required) {
        m_passthroughChild = child;
        m_passthroughFailureChild = 0;
        return;
    }

    ::SetLastError(0);
    const LONG_PTR previous =
        ::SetWindowLongPtr(hwnd, GWL_EXSTYLE, current | required);
    const DWORD error = ::GetLastError();
    if (previous == 0 && error != 0) {
        if (m_passthroughFailureChild != child) {
            qWarning() << "VideoHost: SetWindowLongPtr(WS_EX_TRANSPARENT) failed for mpv child"
                       << child << "error" << error;
            m_passthroughFailureChild = child;
        }
        return;
    }

    m_passthroughFailureChild = 0;
    m_passthroughChild = child;
    qInfo() << "VideoHost: mouse passthrough enabled on mpv child" << child
            << "exstyle" << Qt::hex << static_cast<qulonglong>(current | required);
}
#endif

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

    LONG_PTR exStyle = 0;
    if (m_passthroughChild)
        exStyle = ::GetWindowLongPtr(reinterpret_cast<HWND>(m_passthroughChild), GWL_EXSTYLE);
    lines.append(QStringLiteral("  mouse passthrough hwnd=0x%1 exstyle=0x%2 forwarded_events=%3")
                     .arg(m_passthroughChild, 0, 16)
                     .arg(static_cast<qulonglong>(exStyle), 0, 16)
                     .arg(m_mouseEventCount));
    lines.append(QStringLiteral("  mpv child windows:"));
    ::EnumChildWindows(host, collectChildWindow, reinterpret_cast<LPARAM>(&lines));
#else
    lines.append(QStringLiteral("  child windows: only available on Windows"));
#endif
    return lines.join(QLatin1Char('\n'));
}
