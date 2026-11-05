#include "titlebar.h"

#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QResizeEvent>
#include <QStyle>
#include <QToolTip>

namespace ui {
namespace {

constexpr int kTitleBarHeight = 36;
constexpr int kButtonWidth = 46;
constexpr int kButtonHeight = 32;

QPixmap appIconPixmap()
{
    QPixmap pixmap(20, 20);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0x3B, 0x82, 0xF6));
    painter.drawRoundedRect(QRectF(0.5, 0.5, 19, 19), 5, 5);
    QFont font;
    font.setPixelSize(13);
    font.setWeight(QFont::Bold);
    painter.setFont(font);
    painter.setPen(QColor(0xF8, 0xFA, 0xFC));
    painter.drawText(QRectF(0, 0, 20, 20), Qt::AlignCenter, QStringLiteral("E"));
    return pixmap;
}

QPushButton *makeButton(QWidget *parent, const QString &name, const QString &text,
                        const QString &toolTip)
{
    auto *button = new QPushButton(text, parent);
    button->setObjectName(name);
    button->setProperty("titleBarButton", true);
    button->setFixedSize(kButtonWidth, kButtonHeight);
    button->setFlat(true);
    button->setFocusPolicy(Qt::NoFocus);
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(toolTip);
    return button;
}

} // namespace

TitleBar::TitleBar(QWidget *topLevelWindow, QWidget *parent)
    : QWidget(parent ? parent : topLevelWindow)
    , m_window(topLevelWindow)
{
    setObjectName(QStringLiteral("TitleBar"));
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(kTitleBarHeight);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 0, 8, 0);
    layout->setSpacing(8);

    m_iconLabel = new QLabel(this);
    m_iconLabel->setObjectName(QStringLiteral("TitleBarIcon"));
    m_iconLabel->setFixedSize(20, 20);
    m_iconLabel->setPixmap(appIconPixmap());
    m_iconLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    layout->addWidget(m_iconLabel, 0, Qt::AlignVCenter);

    m_titleLabel = new QLabel(this);
    m_titleLabel->setObjectName(QStringLiteral("TitleBarTitle"));
    m_titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    m_titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    m_titleLabel->setMinimumWidth(0);
    m_titleLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    layout->addWidget(m_titleLabel, 1, Qt::AlignVCenter);

    m_minimizeButton = makeButton(this, QStringLiteral("TitleBarMinimizeButton"),
                                  QStringLiteral("—"), QStringLiteral("最小化"));
    m_maximizeButton = makeButton(this, QStringLiteral("TitleBarMaximizeButton"),
                                  QStringLiteral("□"), QStringLiteral("最大化"));
    m_closeButton = makeButton(this, QStringLiteral("TitleBarCloseButton"),
                               QStringLiteral("✕"), QStringLiteral("关闭"));
    layout->addWidget(m_minimizeButton, 0, Qt::AlignVCenter);
    layout->addWidget(m_maximizeButton, 0, Qt::AlignVCenter);
    layout->addWidget(m_closeButton, 0, Qt::AlignVCenter);

    connect(m_minimizeButton, &QPushButton::clicked, this, &TitleBar::minimizeRequested);
    connect(m_maximizeButton, &QPushButton::clicked, this, &TitleBar::maximizeToggleRequested);
    connect(m_closeButton, &QPushButton::clicked, this, &TitleBar::closeRequested);

    if (m_window) {
        m_window->installEventFilter(this);
        setTitle(m_window->windowTitle());
    }
    updateMaximizeButton();
}

void TitleBar::setTitle(const QString &title)
{
    m_titleText = title;
    if (m_titleLabel)
        m_titleLabel->setText(title);
}

void TitleBar::setMaximized(bool maximized)
{
    m_maximized = maximized;
    updateMaximizeButton();
}

void TitleBar::updateMaximizeButton()
{
    if (!m_maximizeButton)
        return;
    m_maximizeButton->setText(m_maximized ? QStringLiteral("❐") : QStringLiteral("□"));
    m_maximizeButton->setToolTip(m_maximized ? QStringLiteral("还原") : QStringLiteral("最大化"));
    m_maximizeButton->setAccessibleName(m_maximizeButton->toolTip());
}

QRect TitleBar::maximizeButtonRect() const
{
    return m_maximizeButton ? m_maximizeButton->geometry() : QRect();
}

void TitleBar::emitMaximizeRect()
{
    if (m_maximizeButton) {
        if (auto *layout = this->layout())
            layout->activate();
        emit maximizeButtonRectChanged(m_maximizeButton->geometry());
    }
}

void TitleBar::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    emitMaximizeRect();
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    QWidget::mouseDoubleClickEvent(event);
    if (event->button() == Qt::LeftButton)
        emit maximizeToggleRequested();
}

bool TitleBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_window && event->type() == QEvent::WindowTitleChange) {
        setTitle(m_window->windowTitle());
        return false;
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace ui
