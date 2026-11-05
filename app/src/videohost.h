#pragma once

#include <QPoint>
#include <QPointF>
#include <QString>
#include <QWidget>

class QMouseEvent;
class QTimer;
class QWheelEvent;
class QResizeEvent;
class QShowEvent;

// 承载 mpv 原生子窗口的宿主：内部是一个 native HWND，mpv 会在它下面创建自己的子窗口。
//
// T22：--wid 模式下 mpv 的 VO 子窗口在 mpv 自己的线程创建，跨线程 SetWindowSubclass
// 会失败（实机日志每 120ms 刷 error 0）。现在不再子类化，而是给 mpv 子窗口加
// WS_EX_TRANSPARENT | WS_EX_NOACTIVATE，让鼠标事件穿透到本 Qt 控件，再由 Qt 鼠标
// 事件转成 mpv 的 mouse 命令；键盘靠点击视频后把焦点还给 Qt 主窗口。
class VideoHost : public QWidget
{
    Q_OBJECT
public:
    explicit VideoHost(QWidget *parent = nullptr);
    ~VideoHost() override;

    // 返回宿主 HWND（会强制创建原生窗口）
    quintptr nativeWindow();

    QSize minimumSizeHint() const override;

    // 导出当前宿主与子窗口几何/鼠标穿透信息，供“帮助 → 导出诊断信息”使用。
    QString diagnosticSummary();

signals:
    void mouseMoved(const QPoint &position);
    void mouseButtonPressed(int button, const QPoint &position, bool doubleClick);
    void mouseButtonReleased(int button);
    void mouseWheel(int button, const QPoint &position);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;

private:
    void syncMpvChildGeometry();
    QPoint toMpvPosition(const QPointF &position) const;

#ifdef Q_OS_WIN
    void ensureMousePassthrough(quintptr child);
#endif

    QTimer *m_syncTimer = nullptr;
    quintptr m_passthroughChild = 0;
    quintptr m_passthroughFailureChild = 0;
    int m_mouseEventCount = 0;
};
