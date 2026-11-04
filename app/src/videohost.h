#pragma once

#include <QWidget>

class QTimer;

// 承载 mpv 原生子窗口的宿主：内部是一个 native HWND，mpv 会在它下面创建自己的子窗口。
class VideoHost : public QWidget
{
    Q_OBJECT
public:
    explicit VideoHost(QWidget *parent = nullptr);

    // 返回宿主 HWND（会强制创建原生窗口）
    quintptr nativeWindow();

    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

private:
    void syncMpvChildGeometry();

    QTimer *m_syncTimer = nullptr;
};
