#include "playerpage.h"

#include "icons.h"

#include <QAction>
#include <QDebug>
#include <QActionGroup>
#include <climits>
#include <QCursor>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSlider>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

namespace ui {
namespace {

// T31：播放模式下视频子 HWND 不能贴死顶层客户区，否则 Windows 的 WM_NCHITTEST
// 会落在 mpv 子窗口上，WindowFrame 的四边/四角缩放命中失效。留 5px 给顶层窗口。
constexpr int kPlayerResizeMargin = 5;

QString formatTimeValue(qint64 milliseconds)
{
    if (milliseconds < 0)
        milliseconds = 0;
    const qint64 totalSeconds = milliseconds / 1000;
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds % 3600) / 60;
    const qint64 seconds = totalSeconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

QPushButton *makeIconButton(Icon iconKind, const QString &tooltip, const QString &objectName,
                            QWidget *parent)
{
    auto *button = new QPushButton(parent);
    button->setObjectName(objectName);
    button->setProperty("role", QStringLiteral("playerIcon"));
    button->setIcon(icon(iconKind, QColor(0xE2, 0xE8, 0xF0), 22));
    button->setIconSize(QSize(22, 22));
    button->setFixedSize(40, 40);
    button->setToolTip(tooltip);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

} // namespace

// 视频宿主占位：黑底 + 虚线框 + 提示文字。真正 mpv 窗口由 attachVideoSurface 替换。
class VideoHostPlaceholder : public QWidget
{
public:
    explicit VideoHostPlaceholder(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("VideoHostPlaceholder"));
        setMinimumSize(320, 180);

        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(40, 40, 40, 40);
        layout->setSpacing(12);
        layout->addStretch(1);

        auto *iconLabel = new QLabel(this);
        iconLabel->setAlignment(Qt::AlignCenter);
        iconLabel->setPixmap(iconPixmap(Icon::Play, QColor(0x47, 0x55, 0x69), 44));
        layout->addWidget(iconLabel);

        auto *title = new QLabel(QStringLiteral("VideoHost 占位"), this);
        title->setObjectName(QStringLiteral("VideoHostTitle"));
        title->setAlignment(Qt::AlignCenter);
        layout->addWidget(title);

        auto *desc = new QLabel(
            QStringLiteral("实际播放窗口由主窗口 / 播放器模块调用 attachVideoSurface() 挂载"), this);
        desc->setObjectName(QStringLiteral("VideoHostDesc"));
        desc->setAlignment(Qt::AlignCenter);
        layout->addWidget(desc, 0, Qt::AlignHCenter);

        auto *hint = new QLabel(QStringLiteral("Windows 下为 libmpv --wid 的原生子 HWND"), this);
        hint->setObjectName(QStringLiteral("VideoHostHint"));
        hint->setAlignment(Qt::AlignCenter);
        layout->addWidget(hint, 0, Qt::AlignHCenter);
        layout->addStretch(1);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(0, 0, 0));
    }
};

PlayerPage::PlayerPage(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("PlayerPage"));
    setAttribute(Qt::WA_StyledBackground, true);
    setMouseTracking(true); // T35：边缘 5px 需要 mouseMove 更新缩放光标

    // EMBY_PC_PLAYER_OVERLAY=0 时退回页面内浮层（排障/无合成器环境）。
    const QString overlayEnv = qEnvironmentVariable("EMBY_PC_PLAYER_OVERLAY");
    m_topLevelOverlay = overlayEnv != QLatin1String("0");

    buildUi();

    m_overlayTimer = new QTimer(this);
    m_overlayTimer->setInterval(150);
    connect(m_overlayTimer, &QTimer::timeout, this, &PlayerPage::pollOverlayCursor);
    m_lastInteraction.start();
    hideOverlays();
}

PlayerPage::~PlayerPage() = default;

void PlayerPage::createOverlayWindows()
{
    const Qt::WindowFlags overlayFlags =
        Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus;
    auto makeOverlay = [this, overlayFlags](const QString &objectName) {
        QWidget *overlay = m_topLevelOverlay ? new QWidget(this, overlayFlags) : new QWidget(this);
        overlay->setObjectName(objectName);
        overlay->setAttribute(Qt::WA_StyledBackground, true);
        if (m_topLevelOverlay) {
            overlay->setAttribute(Qt::WA_TranslucentBackground, true);
            overlay->setAttribute(Qt::WA_ShowWithoutActivating, true);
            overlay->setAttribute(Qt::WA_NoSystemBackground, true);
            overlay->setFocusPolicy(Qt::NoFocus);
        }
        overlay->hide();
        return overlay;
    };
    m_topBar = makeOverlay(QStringLiteral("PlayerTopBar"));
    m_controlBar = makeOverlay(QStringLiteral("PlayerControlBar"));
}

void PlayerPage::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    createOverlayWindows();

    // 顶部信息栏：不加入布局，作为浮层压在视频上。
    auto *topLayout = new QHBoxLayout(m_topBar);
    topLayout->setContentsMargins(14, 8, 20, 8);
    topLayout->setSpacing(12);
    m_backButton = makeIconButton(Icon::Back, QStringLiteral("返回"), QStringLiteral("PlayerBackButton"),
                                  m_topBar);
    topLayout->addWidget(m_backButton);

    auto *titles = new QVBoxLayout();
    titles->setSpacing(1);
    m_titleLabel = new QLabel(QStringLiteral("未选择影片"), m_topBar);
    m_titleLabel->setObjectName(QStringLiteral("PlayerTitle"));
    m_subtitleLabel = new QLabel(QStringLiteral("—"), m_topBar);
    m_subtitleLabel->setObjectName(QStringLiteral("PlayerSubtitle"));
    titles->addWidget(m_titleLabel);
    titles->addWidget(m_subtitleLabel);
    topLayout->addLayout(titles);
    topLayout->addStretch(1);

    auto *modeBadge = new QLabel(QStringLiteral("演示模式"), m_topBar);
    modeBadge->setObjectName(QStringLiteral("PlayerModeBadge"));
    modeBadge->setFixedHeight(24);
    modeBadge->setAlignment(Qt::AlignCenter);
    topLayout->addWidget(modeBadge, 0, Qt::AlignVCenter);

    // 视频区域
    m_surfaceStack = new QStackedWidget(this);
    m_surfaceStack->setObjectName(QStringLiteral("VideoArea"));
    m_surfaceStack->installEventFilter(this); // T35：VideoArea 边缘双击/按压兜底
    m_placeholder = new VideoHostPlaceholder(m_surfaceStack);
    m_surfaceStack->addWidget(m_placeholder);
    root->addWidget(m_surfaceStack, 1);

    // 底部控制条：同样作为浮层，不占视频布局高度。
    auto *controls = new QVBoxLayout(m_controlBar);
    controls->setContentsMargins(22, 10, 22, 12);
    controls->setSpacing(6);

    auto *seekRow = new QHBoxLayout();
    seekRow->setSpacing(12);
    m_positionLabel = new QLabel(QStringLiteral("00:00"), m_controlBar);
    m_positionLabel->setObjectName(QStringLiteral("PlayerTimeLabel"));
    m_positionLabel->setMinimumWidth(48);
    m_durationLabel = new QLabel(QStringLiteral("00:00"), m_controlBar);
    m_durationLabel->setObjectName(QStringLiteral("PlayerTimeLabel"));
    m_durationLabel->setMinimumWidth(48);
    m_durationLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_seekSlider = new QSlider(Qt::Horizontal, m_controlBar);
    m_seekSlider->setObjectName(QStringLiteral("SeekSlider"));
    m_seekSlider->setRange(0, 0);
    m_seekSlider->setCursor(Qt::PointingHandCursor);
    seekRow->addWidget(m_positionLabel);
    seekRow->addWidget(m_seekSlider, 1);
    seekRow->addWidget(m_durationLabel);
    controls->addLayout(seekRow);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(6);
    m_playButton = new QPushButton(m_controlBar);
    m_playButton->setObjectName(QStringLiteral("PlayPauseButton"));
    m_playButton->setProperty("role", QStringLiteral("playerPrimary"));
    m_playButton->setIcon(icon(Icon::Play, QColor(0xFF, 0xFF, 0xFF), 24));
    m_playButton->setIconSize(QSize(24, 24));
    m_playButton->setFixedSize(46, 46);
    m_playButton->setToolTip(QStringLiteral("播放 / 暂停"));
    m_playButton->setCursor(Qt::PointingHandCursor);
    m_prevButton = makeIconButton(Icon::Prev, QStringLiteral("上一集"), QStringLiteral("PrevButton"),
                                  m_controlBar);
    m_nextButton = makeIconButton(Icon::Next, QStringLiteral("下一集"), QStringLiteral("NextButton"),
                                  m_controlBar);
    buttonRow->addWidget(m_playButton);
    buttonRow->addWidget(m_prevButton);
    buttonRow->addWidget(m_nextButton);
    buttonRow->addStretch(1);

    m_subtitleButton = new QPushButton(icon(Icon::Subtitle, QColor(0xE2, 0xE8, 0xF0), 18),
                                       QStringLiteral("字幕"), m_controlBar);
    m_subtitleButton->setObjectName(QStringLiteral("SubtitleButton"));
    m_subtitleButton->setProperty("role", QStringLiteral("playerText"));
    m_subtitleButton->setCursor(Qt::PointingHandCursor);
    m_audioButton = new QPushButton(icon(Icon::Audio, QColor(0xE2, 0xE8, 0xF0), 18),
                                    QStringLiteral("音轨"), m_controlBar);
    m_audioButton->setObjectName(QStringLiteral("AudioButton"));
    m_audioButton->setProperty("role", QStringLiteral("playerText"));
    m_audioButton->setCursor(Qt::PointingHandCursor);
    m_statsButton = new QPushButton(icon(Icon::Stats, QColor(0xE2, 0xE8, 0xF0), 18),
                                    QStringLiteral("统计"), m_controlBar);
    m_statsButton->setObjectName(QStringLiteral("StatsButton"));
    m_statsButton->setProperty("role", QStringLiteral("playerText"));
    m_statsButton->setCursor(Qt::PointingHandCursor);
    buttonRow->addWidget(m_subtitleButton);
    buttonRow->addWidget(m_audioButton);
    buttonRow->addWidget(m_statsButton);
    buttonRow->addSpacing(10);

    auto *volumeIcon = new QLabel(m_controlBar);
    volumeIcon->setPixmap(iconPixmap(Icon::Volume, QColor(0x94, 0xA3, 0xB8), 20));
    m_volumeSlider = new QSlider(Qt::Horizontal, m_controlBar);
    m_volumeSlider->setObjectName(QStringLiteral("VolumeSlider"));
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(80);
    m_volumeSlider->setFixedWidth(96);
    m_volumeSlider->setCursor(Qt::PointingHandCursor);
    buttonRow->addWidget(volumeIcon);
    buttonRow->addWidget(m_volumeSlider);
    buttonRow->addSpacing(6);

    m_fullscreenButton = makeIconButton(Icon::Fullscreen, QStringLiteral("全屏"),
                                        QStringLiteral("FullscreenButton"), m_controlBar);
    buttonRow->addWidget(m_fullscreenButton);
    controls->addLayout(buttonRow);

    connect(m_backButton, &QPushButton::clicked, this, &PlayerPage::backRequested);
    connect(m_playButton, &QPushButton::clicked, this, &PlayerPage::playPauseRequested);
    connect(m_prevButton, &QPushButton::clicked, this, [this]() {
        qInfo() << "PlayerPage: PrevButton clicked enabled=" << m_prevButton->isEnabled()
                << "playerMode=" << m_playerModeActive;
        emit previousEpisodeRequested();
    });
    connect(m_nextButton, &QPushButton::clicked, this, [this]() {
        qInfo() << "PlayerPage: NextButton clicked enabled=" << m_nextButton->isEnabled()
                << "playerMode=" << m_playerModeActive;
        emit nextEpisodeRequested();
    });
    connect(m_subtitleButton, &QPushButton::clicked, this,
            [this]() { showTrackMenu(m_subtitleButton, m_subtitleTracks, false); });
    connect(m_audioButton, &QPushButton::clicked, this,
            [this]() { showTrackMenu(m_audioButton, m_audioTracks, true); });
    connect(m_statsButton, &QPushButton::clicked, this, &PlayerPage::statsRequested);
    connect(m_fullscreenButton, &QPushButton::clicked, this, [this]() {
        const bool wasFullScreen = window() && window()->isFullScreen();
        qInfo() << "PlayerPage: FullscreenButton clicked fullscreen=" << m_fullscreen
                << "playerMode=" << m_playerModeActive
                << "windowFullScreen=" << wasFullScreen;
        emit fullscreenRequested();
        // 兜底：若 UI 层的 fullscreenRequested 连接缺失/被意外断开，直接调用
        // MainWindow 的 Q_INVOKABLE 槽一次；正常连接已改变状态时不会重复切换。
        QTimer::singleShot(0, this, [this, wasFullScreen]() {
            if (!window() || window()->isFullScreen() != wasFullScreen)
                return;
            qWarning() << "PlayerPage: fullscreenRequested did not change window state; "
                          "invoking MainWindow::toggleFullscreen fallback";
            const bool invoked = QMetaObject::invokeMethod(window(), "toggleFullscreen",
                                                           Qt::DirectConnection);
            qInfo() << "PlayerPage: fullscreen fallback invoked=" << invoked;
        });
    });
    connect(m_volumeSlider, &QSlider::valueChanged, this, &PlayerPage::volumeChanged);
    connect(m_seekSlider, &QSlider::sliderMoved, this, [this](int value) {
        if (!m_updatingSlider)
            m_positionLabel->setText(formatTimeValue(value));
    });
    connect(m_seekSlider, &QSlider::sliderReleased, this, [this]() {
        if (!m_updatingSlider)
            emit seekRequested(m_seekSlider->value());
    });

    if (!m_topLevelOverlay)
        updateDockedOverlayGeometry();

    // 初始没有剧集队列上下文，避免用户误点。
    setEpisodeNavigationEnabled(false, false);
}

Qt::Edges PlayerPage::edgeResizeEdgesAt(const QPoint &position) const
{
    // 只在实际播放器模式且非全屏时启用；全屏没有 5px 边距。
    if (!m_playerModeActive || m_fullscreen)
        return {};
    if (!rect().contains(position))
        return {};
    const int margin = kPlayerResizeMargin;
    Qt::Edges edges;
    if (position.x() < margin)
        edges |= Qt::LeftEdge;
    if (position.x() >= width() - margin)
        edges |= Qt::RightEdge;
    if (position.y() < margin)
        edges |= Qt::TopEdge;
    if (position.y() >= height() - margin)
        edges |= Qt::BottomEdge;
    return edges;
}

bool PlayerPage::tryStartEdgeResize(const QPoint &globalPosition)
{
    const Qt::Edges edges = edgeResizeEdgesAt(mapFromGlobal(globalPosition));
    if (!edges)
        return false;
    QWindow *handle = window() ? window()->windowHandle() : nullptr;
    const bool started = handle && handle->startSystemResize(edges);
    qInfo() << "PlayerPage::edgeResize: edges" << static_cast<int>(edges)
            << "global" << globalPosition << "startSystemResize" << started;
    return true;
}

void PlayerPage::mouseMoveEvent(QMouseEvent *event)
{
    const Qt::Edges edges = edgeResizeEdgesAt(event->position().toPoint());
    if (!edges) {
        unsetCursor();
    } else if ((edges.testFlag(Qt::LeftEdge) && edges.testFlag(Qt::TopEdge))
               || (edges.testFlag(Qt::RightEdge) && edges.testFlag(Qt::BottomEdge))) {
        setCursor(Qt::SizeFDiagCursor);
    } else if ((edges.testFlag(Qt::RightEdge) && edges.testFlag(Qt::TopEdge))
               || (edges.testFlag(Qt::LeftEdge) && edges.testFlag(Qt::BottomEdge))) {
        setCursor(Qt::SizeBDiagCursor);
    } else if (edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge)) {
        setCursor(Qt::SizeHorCursor);
    } else {
        setCursor(Qt::SizeVerCursor);
    }
    QWidget::mouseMoveEvent(event);
}

void PlayerPage::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton
        && tryStartEdgeResize(event->globalPosition().toPoint())) {
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

void PlayerPage::leaveEvent(QEvent *event)
{
    unsetCursor();
    QWidget::leaveEvent(event);
}

void PlayerPage::paintEvent(QPaintEvent *event)
{
    QWidget::paintEvent(event);
    // 播放模式的 5px 可缩放边缘保持纯黑，避免主题底色或视频边缘露出杂色。
    if (m_playerModeActive && !m_fullscreen) {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
    }
}

void PlayerPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_topLevelOverlay) {
        if (m_playerModeActive)
            syncOverlayGeometry();
    } else {
        updateDockedOverlayGeometry();
    }
}

void PlayerPage::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    ensureHostWindow();
    if (m_playerModeActive) {
        m_lastCursorPos = currentCursorPosition();
        m_lastInteraction.restart();
        showOverlays();
        if (m_autoHide)
            startAutoHideTimer();
    }
}

void PlayerPage::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    hideOverlays();
    stopAutoHideTimer();
}

bool PlayerPage::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_surface || watched == m_surfaceStack) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton
                && tryStartEdgeResize(mouseEvent->globalPosition().toPoint())) {
                mouseEvent->accept();
                return true;
            }
        }
    }

    if (watched == m_hostWindow) {
        switch (event->type()) {
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::ScreenChangeInternal:
            if (m_playerModeActive)
                syncOverlayGeometry();
            break;
        case QEvent::Show:
            if (m_playerModeActive) {
                showOverlays();
                if (m_autoHide)
                    startAutoHideTimer();
            }
            break;
        case QEvent::Hide:
        case QEvent::Close:
            hideOverlays();
            stopAutoHideTimer();
            break;
        case QEvent::WindowStateChange:
            if (m_hostWindow && m_hostWindow->isMinimized()) {
                hideOverlays();
                stopAutoHideTimer();
            } else if (m_playerModeActive) {
                showOverlays();
                if (m_autoHide)
                    startAutoHideTimer();
            }
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void PlayerPage::ensureHostWindow()
{
    QWidget *host = window();
    if (host == this)
        host = nullptr;
    if (host == m_hostWindow)
        return;
    if (m_hostWindow)
        m_hostWindow->removeEventFilter(this);
    m_hostWindow = host;
    if (m_hostWindow)
        m_hostWindow->installEventFilter(this);
}

QRect PlayerPage::hostGlobalRect() const
{
    QWidget *host = m_hostWindow ? m_hostWindow : window();
    if (!host || host == this)
        return QRect(mapToGlobal(QPoint(0, 0)), size());
    return QRect(host->mapToGlobal(QPoint(0, 0)), host->size());
}

void PlayerPage::applyVideoMargins()
{
    auto *layout = qobject_cast<QVBoxLayout *>(this->layout());
    if (!layout)
        return;
    const int margin = (m_playerModeActive && !m_fullscreen) ? kPlayerResizeMargin : 0;
    layout->setContentsMargins(margin, margin, margin, margin);
    if (m_topLevelOverlay && m_playerModeActive)
        syncOverlayGeometry();
    else if (!m_topLevelOverlay)
        updateDockedOverlayGeometry();
}

void PlayerPage::syncOverlayGeometry()
{
    if (!m_topBar || !m_controlBar)
        return;
    if (!m_topLevelOverlay) {
        updateDockedOverlayGeometry();
        return;
    }

    ensureHostWindow();
    const QRect host = hostGlobalRect();
    if (!host.isValid() || host.isEmpty())
        return;

    const int margin = (m_playerModeActive && !m_fullscreen) ? kPlayerResizeMargin : 0;
    const int topHeight = 76;
    const int maxTopWidth = qMax(200, host.width() - 24 - 2 * margin);
    const int topWidth = qMin(qMax(260, m_topBar->sizeHint().width()), maxTopWidth);
    m_topBar->setGeometry(host.left() + 12 + margin, host.top() + 10 + margin, topWidth, topHeight);

    // T31：底部 overlay 同步内缩，避免盖住窗口底边的可缩放边缘；全屏 margin=0。
    const int controlHeight = 96;
    m_controlBar->setGeometry(host.left() + margin,
                              host.top() + host.height() - controlHeight - margin,
                              qMax(0, host.width() - 2 * margin), controlHeight);

    if (m_overlaysVisible) {
        m_topBar->raise();
        m_controlBar->raise();
    }
}

void PlayerPage::updateDockedOverlayGeometry()
{
    if (!m_topBar || !m_controlBar)
        return;
    const int margin = (m_playerModeActive && !m_fullscreen) ? kPlayerResizeMargin : 0;
    const int topHeight = qBound(58, height() / 9, 84);
    const int controlHeight = qBound(96, height() / 7, 112);
    m_topBar->setGeometry(margin, margin, qMax(0, width() - 2 * margin), topHeight);
    m_controlBar->setGeometry(margin, height() - controlHeight - margin,
                              qMax(0, width() - 2 * margin), controlHeight);
    if (m_overlaysVisible) {
        m_topBar->raise();
        m_controlBar->raise();
    }
}

void PlayerPage::showOverlays()
{
    if (!m_controlsRequested || !m_playerModeActive || !isVisible())
        return;
    syncOverlayGeometry();
    m_topBar->show();
    m_controlBar->show();
    m_overlaysVisible = true;
    m_topBar->raise();
    m_controlBar->raise();
}

void PlayerPage::hideOverlays()
{
    m_overlaysVisible = false;
    if (m_topBar)
        m_topBar->hide();
    if (m_controlBar)
        m_controlBar->hide();
}

void PlayerPage::applyOverlayVisibility()
{
    if (m_controlsRequested && m_playerModeActive)
        showOverlays();
    else
        hideOverlays();
}

void PlayerPage::setPlayerModeActive(bool active)
{
    if (m_playerModeActive == active) {
        if (active) {
            showOverlays();
            if (m_autoHide)
                startAutoHideTimer();
        }
        return;
    }

    m_playerModeActive = active;
    applyVideoMargins();
    if (active) {
        ensureHostWindow();
        m_controlsRequested = true;
        m_lastCursorPos = currentCursorPosition();
        m_lastInteraction.restart();
        showOverlays();
        if (m_autoHide)
            startAutoHideTimer();
        else
            stopAutoHideTimer();
    } else {
        stopAutoHideTimer();
        hideOverlays();
        applyVideoMargins();
    }
}

void PlayerPage::setControlsVisible(bool visible)
{
    m_controlsRequested = visible;
    if (!visible) {
        stopAutoHideTimer();
        hideOverlays();
        return;
    }
    if (!m_playerModeActive)
        return;
    m_lastInteraction.restart();
    showOverlays();
    if (m_autoHide)
        startAutoHideTimer();
}

void PlayerPage::setOverlayAutoHide(bool enabled)
{
    m_autoHide = enabled;
    if (!enabled)
        stopAutoHideTimer();
    if (m_playerModeActive && m_controlsRequested) {
        showOverlays();
        m_lastInteraction.restart();
        if (enabled)
            startAutoHideTimer();
    }
}

void PlayerPage::setOverlayHideDelay(int milliseconds)
{
    m_hideDelayMs = qMax(0, milliseconds);
}

void PlayerPage::setOverlayCursorPositionForTest(const QPoint &globalPos, bool hasPosition)
{
    m_hasTestCursor = hasPosition;
    m_testCursorPos = globalPos;
    if (hasPosition)
        pollOverlayCursor();
}

QPoint PlayerPage::currentCursorPosition() const
{
    return m_hasTestCursor ? m_testCursorPos : QCursor::pos();
}

void PlayerPage::startAutoHideTimer()
{
    if (!m_overlayTimer)
        return;
    if (m_autoHide && m_playerModeActive)
        m_overlayTimer->start();
}

void PlayerPage::stopAutoHideTimer()
{
    if (m_overlayTimer)
        m_overlayTimer->stop();
}

void PlayerPage::pollOverlayCursor()
{
    if (!m_playerModeActive || !m_autoHide || !m_controlsRequested)
        return;

    const QPoint position = currentCursorPosition();
    const QRect host = hostGlobalRect();
    const bool inside = host.contains(position);

    // 鼠标在主窗口内移动：显示浮层并重置隐藏计时。
    if (inside && position != m_lastCursorPos) {
        m_lastCursorPos = position;
        m_lastInteraction.restart();
        showOverlays();
        return;
    }
    if (!inside)
        m_lastCursorPos = position;

    // 暂停时保持显示（常见播放器行为）。
    if (!m_playing) {
        showOverlays();
        return;
    }

    if (m_lastInteraction.isValid() && m_lastInteraction.elapsed() >= m_hideDelayMs)
        hideOverlays();
}

void PlayerPage::attachVideoSurface(QWidget *surface)
{
    // T15：真实播放时也必须保留 Qt 控制条；mpv OSC 在部分 libmpv 构建里不可用。
    if (m_playerModeActive)
        showOverlays();
    else
        syncOverlayGeometry();
    if (m_surface == surface)
        return;
    if (m_surface) {
        m_surface->removeEventFilter(this);
        m_surfaceStack->removeWidget(m_surface);
        m_surface->hide();
        m_surface->setParent(nullptr);
        m_surface = nullptr;
    }
    if (!surface) {
        m_surfaceStack->setCurrentWidget(m_placeholder);
        return;
    }
    m_surface = surface;
    m_surface->installEventFilter(this); // T35：子控件覆盖时也能命中边缘
    m_surfaceStack->addWidget(surface);
    m_surfaceStack->setCurrentWidget(surface);
    surface->show();
}

void PlayerPage::releaseVideoSurface()
{
    attachVideoSurface(nullptr);
}

QWidget *PlayerPage::videoSurfaceContainer() const
{
    return m_surfaceStack;
}

void PlayerPage::setEpisodeInfo(const QString &title, const QString &subtitle)
{
    m_titleLabel->setText(title);
    m_subtitleLabel->setText(subtitle);
    if (m_playerModeActive)
        syncOverlayGeometry();
}

void PlayerPage::setPosition(qint64 positionMs, qint64 durationMs)
{
    m_durationMs = qMax<qint64>(0, durationMs);
    const qint64 position = qBound<qint64>(0, positionMs, m_durationMs);

    m_updatingSlider = true;
    m_seekSlider->setRange(0, static_cast<int>(qMin<qint64>(m_durationMs, INT_MAX)));
    m_seekSlider->setValue(static_cast<int>(position));
    m_updatingSlider = false;

    m_positionLabel->setText(formatTimeValue(position));
    m_durationLabel->setText(formatTimeValue(m_durationMs));
}

void PlayerPage::setPlaying(bool playing)
{
    if (m_playing == playing)
        return;
    m_playing = playing;
    m_playButton->setIcon(icon(playing ? Icon::Pause : Icon::Play, QColor(0xFF, 0xFF, 0xFF), 24));
    m_playButton->setToolTip(playing ? QStringLiteral("暂停") : QStringLiteral("播放"));

    if (!m_playerModeActive)
        return;
    if (playing) {
        m_lastInteraction.restart();
        if (m_autoHide)
            startAutoHideTimer();
    } else {
        // 暂停时保持控制条可见，方便继续播放/选集。
        showOverlays();
        stopAutoHideTimer();
    }
}

void PlayerPage::setSubtitleTracks(const QList<TrackInfo> &tracks)
{
    m_subtitleTracks = tracks;
    for (const TrackInfo &track : tracks) {
        if (track.selected && !track.title.isEmpty()) {
            m_subtitleButton->setText(QStringLiteral("字幕 · %1").arg(track.title));
            return;
        }
    }
    m_subtitleButton->setText(QStringLiteral("字幕"));
}

void PlayerPage::setAudioTracks(const QList<TrackInfo> &tracks)
{
    m_audioTracks = tracks;
    for (const TrackInfo &track : tracks) {
        if (track.selected && !track.title.isEmpty()) {
            m_audioButton->setText(QStringLiteral("音轨 · %1").arg(track.title));
            return;
        }
    }
    m_audioButton->setText(QStringLiteral("音轨"));
}

void PlayerPage::setVolume(int volume)
{
    const int clamped = qBound(0, volume, 100);
    if (m_volumeSlider->value() == clamped)
        return;
    m_volumeSlider->blockSignals(true);
    m_volumeSlider->setValue(clamped);
    m_volumeSlider->blockSignals(false);
}

int PlayerPage::volume() const
{
    return m_volumeSlider->value();
}

void PlayerPage::setEpisodeNavigationEnabled(bool hasPrevious, bool hasNext)
{
    if (m_prevButton) {
        m_prevButton->setEnabled(hasPrevious);
        m_prevButton->setToolTip(hasPrevious ? QStringLiteral("上一集")
                                             : QStringLiteral("没有上一集"));
    }
    if (m_nextButton) {
        m_nextButton->setEnabled(hasNext);
        m_nextButton->setToolTip(hasNext ? QStringLiteral("下一集")
                                         : QStringLiteral("没有下一集"));
    }
    qInfo() << "PlayerPage: episode navigation updated prev=" << hasPrevious
            << "next=" << hasNext;
}

void PlayerPage::setFullscreen(bool fullscreen)
{
    qInfo() << "PlayerPage: setFullscreen" << fullscreen
            << "playerMode=" << m_playerModeActive
            << "windowFullScreen=" << (window() && window()->isFullScreen());
    m_fullscreen = fullscreen;
    applyVideoMargins();
    if (m_playerModeActive)
        syncOverlayGeometry();
    m_fullscreenButton->setIcon(icon(fullscreen ? Icon::ExitFullscreen : Icon::Fullscreen,
                                     QColor(0xE2, 0xE8, 0xF0), 22));
    m_fullscreenButton->setToolTip(fullscreen ? QStringLiteral("退出全屏") : QStringLiteral("全屏"));
}

void PlayerPage::showTrackMenu(QPushButton *button, const QList<TrackInfo> &tracks, bool audio)
{
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("TrackMenu"));
    if (tracks.isEmpty()) {
        QAction *empty = menu.addAction(QStringLiteral("没有可用的轨道"));
        empty->setEnabled(false);
        menu.exec(button->mapToGlobal(QPoint(0, -menu.sizeHint().height())));
        return;
    }

    QActionGroup group(&menu);
    group.setExclusive(true);
    for (const TrackInfo &track : tracks) {
        QString text = track.title;
        if (!track.codec.isEmpty())
            text += QStringLiteral("  ·  %1").arg(track.codec);
        QAction *action = menu.addAction(text);
        action->setCheckable(true);
        action->setChecked(track.selected);
        action->setData(track.id);
        group.addAction(action);
    }

    QAction *chosen = menu.exec(button->mapToGlobal(QPoint(0, -menu.sizeHint().height())));
    if (!chosen || !chosen->data().isValid())
        return;
    const QString id = chosen->data().toString();
    if (audio)
        emit audioTrackSelected(id);
    else
        emit subtitleTrackSelected(id);
}

} // namespace ui