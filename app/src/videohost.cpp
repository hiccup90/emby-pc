#include "videohost.h"

#include <QPalette>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

VideoHost::VideoHost(QWidget *parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_NativeWindow);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAutoFillBackground(true);

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
    syncMpvChildGeometry();
}

void VideoHost::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    syncMpvChildGeometry();
}

void VideoHost::syncMpvChildGeometry()
{
#ifdef Q_OS_WIN
    const HWND host = reinterpret_cast<HWND>(winId());
    if (!host)
        return;

    // mpv 会把它自己的窗口挂成我们的子窗口；找到它并跟随宿主尺寸。
    HWND child = ::GetWindow(host, GW_CHILD);
    if (!child)
        return;

    RECT hostRect{};
    if (!::GetClientRect(host, &hostRect))
        return;

    const int w = hostRect.right - hostRect.left;
    const int h = hostRect.bottom - hostRect.top;
    if (w <= 0 || h <= 0)
        return;

    RECT childRect{};
    ::GetClientRect(child, &childRect);
    if (childRect.right != w || childRect.bottom != h) {
        ::MoveWindow(child, 0, 0, w, h, TRUE);
    }
#endif
}
