#include "videohost.h"

#include <QDebug>
#include <QMouseEvent>
#include <QPalette>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStringList>
#include <QWheelEvent>

#include <utility>

#include <rhi/qrhi.h>
#ifdef Q_OS_WIN
#include <rhi/qrhi_platform.h>
#endif

VideoHost::VideoHost(QWidget *parent)
    : QRhiWidget(parent)
{
#ifdef Q_OS_WIN
    // 显式选 D3D11：Qt 6.8 在 Windows 上默认也是 D3D11，但这里不能依赖默认值，
    // 因为 MpvPlayer 的 render context 会固定使用这台 device。
    setApi(QRhiWidget::Api::Direct3D11);
#endif
    // mpv_d3d11_fbo 的约束：2D、非 MSAA、非 array/mipmap；RGBA8 → DXGI
    // B8G8R8A8/R8G8B8A8 UNORM，且 RenderTarget 纹理已带 D3D11_BIND_RENDER_TARGET。
    setSampleCount(1);
    setColorBufferFormat(QRhiWidget::TextureFormat::RGBA8);

    // 不再需要 WA_NativeWindow：它会让父窗口不启用 RHI 合成
    // （q_evaluateRhiConfigRecursive 对 native child 直接 skip），QRhiWidget 就
    // 拿不到 Qt 的 D3D11 device，也没有可合成的纹理。
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMouseTracking(true);

    QPalette pal = palette();
    pal.setColor(QPalette::Window, Qt::black);
    setPalette(pal);
}

VideoHost::~VideoHost()
{
    // 只清回调，不持有 device/texture；render context 的释放归 MpvPlayer
    // （mpv_render_context_free 在 mpv_terminate_destroy 之前，见 MpvPlayer::shutdown）。
    m_frameRenderer = {};
}

void VideoHost::setFrameRenderer(FrameRenderer renderer)
{
    m_frameRenderer = std::move(renderer);
}

QSize VideoHost::minimumSizeHint() const
{
    return QSize(320, 180);
}

void VideoHost::requestFrame()
{
    m_framePending = true;
    update();
}

void VideoHost::captureD3D11Device()
{
#ifdef Q_OS_WIN
    QRhi *rhiBackend = rhi();
    if (!rhiBackend)
        return;
    const auto *handles =
        static_cast<const QRhiD3D11NativeHandles *>(rhiBackend->nativeHandles());
    if (!handles || !handles->dev)
        return;

    const quintptr device = reinterpret_cast<quintptr>(handles->dev);
    if (device == m_device)
        return;
    m_device = device;
    qInfo() << "VideoHost: QRhi D3D11 device" << Qt::hex << m_device
            << "backend" << rhiBackend->backendName();
    // PlayerController 收到后交给 mpv_render_context_create(D3D11_INIT_PARAMS)。
    // 队列连接：不在 Qt 的 paint/initialize 里同步做 mpv 初始化。
    emit d3d11DeviceReady(m_device);
#endif
}

void VideoHost::initialize(QRhiCommandBuffer *cb)
{
    Q_UNUSED(cb);
    captureD3D11Device();
    // QRhi 重建了纹理/换过 RHI：强制下一帧让 mpv 重画。
    m_textureObject = 0;
    m_textureSize = QSize();
    m_textureInitialized = false;
    m_framePending = true;
    m_forceNextFrame = true;
}

void VideoHost::render(QRhiCommandBuffer *cb)
{
#ifdef Q_OS_WIN
    captureD3D11Device();

    QRhiTexture *texture = colorTexture();
    if (!texture)
        return;
    // QRhiWidget 的 colorTexture 由 Qt 创建并直接交给 backingstore 合成
    // （QRhiWidgetPrivate::texture()）。同一个 device 上，D3D11 后端可以直接把
    // 它作为 mpv 的 FBO，省掉“mpv 纹理 → 全屏 quad”的额外一遍采样。
    const QRhiTexture::NativeTexture native = texture->nativeTexture();
    if (!native.object)
        return;

    const QSize textureSize = texture->pixelSize();
    // D3D11 纹理重建后 COM 指针可能复用（同一地址），所以不能只看 object：
    // resize/DPR 变化用 pixelSize 判定，并记下 force 标志。
    const bool textureChanged = native.object != m_textureObject || textureSize != m_textureSize;
    if (textureChanged) {
        m_textureObject = native.object;
        m_textureSize = textureSize;
        m_textureInitialized = false;
        if (qEnvironmentVariableIsSet("EMBY_PC_DEBUG_GEOMETRY")) {
            qInfo() << "VideoHost: composite texture changed" << Qt::hex << m_textureObject
                    << "pixelSize" << textureSize;
        }
    }

    const bool forceRedraw = m_forceNextFrame || textureChanged || !m_textureInitialized;
    const bool shouldRender = m_framePending || forceRedraw;
    m_framePending = false;
    m_forceNextFrame = false;

    if (shouldRender && m_frameRenderer
        && m_frameRenderer(reinterpret_cast<void *>(native.object), forceRedraw)) {
        m_textureInitialized = true;
        return;
    }

    // 只有这一帧没有调用 mpv（render context 未就绪/渲染失败）时，才允许用
    // QRhi 的 pass 把纹理清黑。QRhi D3D11 的命令在 endOffscreenFrame() 才执行，
    // 和 mpv 直接发到 immediate context 的命令混用会把 mpv 的结果盖掉。
    if (!m_textureInitialized && renderTarget()) {
        cb->beginPass(renderTarget(), Qt::black, {1.0f, 0});
        cb->endPass();
        m_textureInitialized = true;
    }
#else
    Q_UNUSED(cb);
#endif
}

void VideoHost::releaseResources()
{
    // 本类不创建 QRhi 资源；只丢掉与旧 RHI/旧纹理绑定的缓存。
    // 新 RHI 会在 initialize()/render() 里重新 capture 并 emit d3d11DeviceReady。
    m_textureObject = 0;
    m_textureSize = QSize();
    m_textureInitialized = false;
    m_framePending = true;
    m_forceNextFrame = true;
    m_device = 0;
}

void VideoHost::resizeEvent(QResizeEvent *event)
{
    QRhiWidget::resizeEvent(event);
    if (qEnvironmentVariableIsSet("EMBY_PC_DEBUG_GEOMETRY"))
        qInfo() << "VideoHost: resize event" << event->size()
                << "dpr" << devicePixelRatioF();
    // QRhiWidget 会重建 colorTexture；render() 靠 textureChanged/force 重画。
    m_forceNextFrame = true;
    requestFrame();
}

void VideoHost::showEvent(QShowEvent *event)
{
    QRhiWidget::showEvent(event);
    if (qEnvironmentVariableIsSet("EMBY_PC_DEBUG_GEOMETRY"))
        qInfo() << "VideoHost: show event" << size();
    m_forceNextFrame = true;
    requestFrame();
}

QPoint VideoHost::toMpvPosition(const QPointF &position) const
{
    // mpv 的 mouse 命令使用视频输出（render target）物理像素；Qt 坐标是逻辑
    // 像素，纹理按 size()*DPR 创建，因此这里乘 DPR。
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
        qInfo() << "VideoHost: mouse double click" << button << position
                << "count" << m_mouseEventCount;
    // doubleClick=true：PlayerController 不再转发给 mpv，而是直接触发
    // PlayerPage::fullscreenRequested（Qt 侧全屏状态机）。
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

QString VideoHost::diagnosticSummary()
{
    QStringList lines;
    lines.append(QStringLiteral("VideoHost(QRhiWidget): geometry=%1x%2 dpr=%3 visible=%4")
                     .arg(width())
                     .arg(height())
                     .arg(devicePixelRatioF(), 0, 'f', 2)
                     .arg(isVisible() ? QStringLiteral("yes") : QStringLiteral("no")));
    lines.append(QStringLiteral("  rhi api=%1 qt=%2 device=0x%3")
                     .arg(static_cast<int>(api()))
                     .arg(QString::fromLatin1(qVersion()))
                     .arg(m_device, 0, 16));
    lines.append(QStringLiteral("  composite texture=0x%1 initialized=%2 pending=%3")
                     .arg(m_textureObject, 0, 16)
                     .arg(m_textureInitialized ? QStringLiteral("yes") : QStringLiteral("no"))
                     .arg(m_framePending ? QStringLiteral("yes") : QStringLiteral("no")));
    const qreal dpr = devicePixelRatioF();
    lines.append(QStringLiteral("  expected pixel size=%1x%2 mouse events=%3 renderer=%4")
                     .arg(qRound(width() * dpr))
                     .arg(qRound(height() * dpr))
                     .arg(m_mouseEventCount)
                     .arg(m_frameRenderer ? QStringLiteral("yes") : QStringLiteral("no")));
    return lines.join(QLatin1Char('\n'));
}
