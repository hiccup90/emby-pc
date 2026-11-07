#pragma once

#include <QPoint>
#include <QPointF>
#include <QRhiWidget>
#include <QSize>
#include <QString>

#include <functional>

class QMouseEvent;
class QPaintEvent;
class QResizeEvent;
class QShowEvent;
class QWheelEvent;

// 视频宿主：spike/render-d3d11 起不再是 mpv VO 子窗口的容器，而是 QRhiWidget。
//
// 职责：
//  - 用 D3D11 后端的 QRhi 拿到 Qt 自己的 ID3D11Device*，交给 MpvPlayer 创建
//    mpv_render_context（同一个 device，d3d11va 也在这上面解码）；
//  - 每一帧把 QRhiWidget 的 colorTexture（ID3D11Texture2D*）交给 mpv 作为
//    MPV_RENDER_PARAM_D3D11_FBO 渲染目标，再由 Qt 的 backingstore 合成上屏；
//  - 鼠标事件转成 mpv `mouse` 命令；双击由 PlayerController 交给 Qt 全屏。
//
// 纹理由 QRhiWidget 创建/重建（尺寸 = size() * devicePixelRatio()），本类不
// 持有 D3D11 资源，也不负责释放 device / texture。
class VideoHost : public QRhiWidget
{
    Q_OBJECT
public:
    explicit VideoHost(QWidget *parent = nullptr);
    ~VideoHost() override;

    // QRhi 的 ID3D11Device*（指针值）；QRhi 尚未初始化时为 0。
    // 只在该对象所属 GUI 线程读取。
    quintptr d3d11Device() const { return m_device; }

    // 把当前帧渲染进 texture（ID3D11Texture2D*）。返回 true 表示这一帧已经
    // 由 mpv 提交到纹理，VideoHost 不会再用 QRhi 命令覆盖它。
    // forceRedraw=true：纹理刚创建/重建（首帧、缩放、DPR 变化），必须重画。
    using FrameRenderer = std::function<bool(void *texture, bool forceRedraw)>;
    void setFrameRenderer(FrameRenderer renderer);

    QSize minimumSizeHint() const override;

    // 导出当前 D3D11/纹理/帧状态，供“帮助 → 导出诊断信息”使用。
    QString diagnosticSummary();

public slots:
    // mpv 有新帧/重绘请求（来自 MpvPlayer::frameUpdate，GUI 线程）。
    void requestFrame();

signals:
    // QRhi 初始化完成或设备变化（Qt 换 RHI 时）。GUI 线程发出。
    void d3d11DeviceReady(quintptr device);
    void mouseMoved(const QPoint &position);
    void mouseButtonPressed(int button, const QPoint &position, bool doubleClick);
    void mouseButtonReleased(int button);
    void mouseWheel(int button, const QPoint &position);

protected:
    void initialize(QRhiCommandBuffer *cb) override;
    void render(QRhiCommandBuffer *cb) override;
    void releaseResources() override;
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    void captureD3D11Device();
    QPoint toMpvPosition(const QPointF &position) const;

    FrameRenderer m_frameRenderer;
    quintptr m_device = 0;
    quint64 m_textureObject = 0;
    QSize m_textureSize;
    bool m_textureInitialized = false;
    bool m_framePending = false;
    bool m_forceNextFrame = false;
    int m_mouseEventCount = 0;
};
