#pragma once

#include <QPoint>
#include <QString>
#include <QWidget>

class QTimer;

// 承载 mpv 原生子窗口的宿主：内部是一个 native HWND，mpv 会在它下面创建自己的子窗口。
class VideoHost : public QWidget
{
    Q_OBJECT
public:
    explicit VideoHost(QWidget *parent = nullptr);
    ~VideoHost() override;

    // 返回宿主 HWND（会强制创建原生窗口）
    quintptr nativeWindow();

    QSize minimumSizeHint() const override;

    // 导出当前宿主与子窗口几何信息，供“帮助 → 导出诊断信息”使用。
    QString diagnosticSummary();

    // Windows 子类化回调转发到 Qt 信号（公开供匿名回调调用）。
    void notifyMouseMove(const QPoint &position);
    void notifyMouseButton(int button, const QPoint &position, bool doubleClick);
    void notifyMouseRelease(int button);
    void notifyMouseWheel(int button, const QPoint &position);
    void notifyKeyPressed(int virtualKey, int modifiers, bool down);
    void notifyChildDestroyed(quintptr child);

signals:
    void mouseMoved(const QPoint &position);
    void mouseButtonPressed(int button, const QPoint &position, bool doubleClick);
    void mouseButtonReleased(int button);
    void mouseWheel(int button, const QPoint &position);
    // 焦点在 mpv 子 HWND 时 Qt 主窗口收不到键盘，子类化回调转发到这里。
    void keyPressed(int virtualKey, int modifiers, bool down);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void syncMpvChildGeometry();
    void installMouseSubclass(quintptr child);

    QTimer *m_syncTimer = nullptr;
    quintptr m_mpvChildHandle = 0;
    int m_mouseEventCount = 0;
    int m_keyEventCount = 0;
};
