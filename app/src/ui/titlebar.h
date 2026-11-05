#pragma once

#include <QRect>
#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;

namespace ui {

// 自绘标题栏（T27）。放在主窗口顶部、QMenuBar 之上；拖动/双击最大化
// 由 WindowFrame 的原生 WM_NCHITTEST/HTCAPTION 处理，按钮走 Qt 点击。
class TitleBar : public QWidget
{
    Q_OBJECT
public:
    explicit TitleBar(QWidget *topLevelWindow, QWidget *parent = nullptr);

    void setTitle(const QString &title);
    QString title() const { return m_titleText; }
    void setMaximized(bool maximized);
    bool isMaximized() const { return m_maximized; }
    QRect maximizeButtonRect() const;

    QPushButton *minimizeButton() const { return m_minimizeButton; }
    QPushButton *maximizeButton() const { return m_maximizeButton; }
    QPushButton *closeButton() const { return m_closeButton; }

signals:
    void minimizeRequested();
    void maximizeToggleRequested();
    void closeRequested();
    // TitleBar 本地逻辑坐标；WindowFrame 用它做精确 HTMAXBUTTON/Snap。
    void maximizeButtonRectChanged(const QRect &rect);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateMaximizeButton();
    void emitMaximizeRect();

    QWidget *m_window = nullptr;
    QLabel *m_iconLabel = nullptr;
    QLabel *m_titleLabel = nullptr;
    QPushButton *m_minimizeButton = nullptr;
    QPushButton *m_maximizeButton = nullptr;
    QPushButton *m_closeButton = nullptr;
    QString m_titleText;
    bool m_maximized = false;
};

} // namespace ui
