#include "videohost.h"

#include <QDebug>
#include <QPalette>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStringList>
#include <QTimer>

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

    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setPalette(pal);

    m_syncTimer = new QTimer(this);
    m_syncTimer->setInterval(120);
    connect(m_syncTimer, &QTimer::timeout, this, &VideoHost::syncMpvChildGeometry);
    m_syncTimer->start();
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
#endif
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
    lines.append(QStringLiteral("  mpv child windows:"));
    ::EnumChildWindows(host, collectChildWindow, reinterpret_cast<LPARAM>(&lines));
#else
    lines.append(QStringLiteral("  child windows: only available on Windows"));
#endif
    return lines.join(QLatin1Char('\n'));
}
