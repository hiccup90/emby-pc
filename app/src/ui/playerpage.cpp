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

#include <functional>

namespace ui {
namespace {

// T31：播放模式下视频子 HWND 不能贴死顶层客户区，否则 Windows 的 WM_NCHITTEST
// 会落在 mpv 子窗口上，WindowFrame 的四边/四角缩放命中失效。留 5px 给顶层窗口。
// T56：边距去掉后，边缘缩放命中区仍保留 6px；布局 margin 不再使用它。
constexpr int kPlayerResizeHit = 6;

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
    button->setIconSize(QSize(20, 20));
    button->setFixedSize(36, 36);
    button->setToolTip(tooltip);
    button->setAccessibleName(tooltip);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

// T45：分段进度条：底轨/缓冲/已播/可拖 handle，保持 QSlider 输入语义。
class SeekSlider : public QSlider
{
public:
    explicit SeekSlider(QWidget *parent = nullptr)
        : QSlider(Qt::Horizontal, parent)
    {
        setMouseTracking(true);
        setFixedHeight(28);
    }

    void setBufferedValue(int value)
    {
        m_bufferedValue = qBound(0, value, maximum());
        update();
    }

    std::function<void(int)> onSeek;

protected:
    void seekAt(qreal x)
    {
        if (maximum() <= minimum())
            return;
        const int px = qBound(0, qRound(x), qMax(1, width()));
        const int value = minimum()
                          + qRound(double(maximum() - minimum()) * px / qMax(1, width()));
        setValue(value);
        update();
        if (onSeek)
            onSeek(value);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_dragging = true;
            seekAt(event->position().x());
            event->accept();
            return;
        }
        QSlider::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_dragging) {
            seekAt(event->position().x());
            event->accept();
            return;
        }
        QSlider::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_dragging) {
            seekAt(event->position().x());
            m_dragging = false;
            event->accept();
            return;
        }
        QSlider::mouseReleaseEvent(event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const int trackHeight = 4;
        const QRectF track(6.0, (height() - trackHeight) / 2.0,
                           qMax(1.0, width() - 12.0), trackHeight);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x33, 0x41, 0x55));
        painter.drawRoundedRect(track, 2, 2);

        const int range = maximum() - minimum();
        const auto fraction = [&](int value) {
            if (range <= 0)
                return 0.0;
            return qBound(0.0, double(value - minimum()) / double(range), 1.0);
        };
        if (m_bufferedValue > value()) {
            QRectF buffered = track;
            buffered.setWidth(track.width() * fraction(m_bufferedValue));
            painter.setBrush(QColor(148, 163, 184, 92));
            painter.drawRoundedRect(buffered, 2, 2);
        }
        QRectF played = track;
        played.setWidth(track.width() * fraction(value()));
        painter.setBrush(QColor(0x3B, 0x82, 0xF6));
        painter.drawRoundedRect(played, 2, 2);

        const qreal handleX = track.left() + track.width() * fraction(value());
        painter.setBrush(QColor(0xF8, 0xFA, 0xFC));
        painter.setPen(QPen(QColor(0x3B, 0x82, 0xF6), 2));
        painter.drawEllipse(QPointF(handleX, track.center().y()), 7, 7);
    }

private:
    int m_bufferedValue = 0;
    bool m_dragging = false;
};

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
            // T55：Windows 分层窗口按像素 alpha 命中，透明区域会穿透导致只有图标中心能点；
            // 控制条改为不透明顶层窗口 + QSS 不透明深色背景，整块按钮都可命中。
            overlay->setAttribute(Qt::WA_TranslucentBackground, false);
            overlay->setAttribute(Qt::WA_NoSystemBackground, false);
            overlay->setAttribute(Qt::WA_ShowWithoutActivating, true);
            overlay->setAutoFillBackground(true);
            overlay->setFocusPolicy(Qt::NoFocus);
        }
        overlay->hide();
        return overlay;
    };
    m_topBar = makeOverlay(QStringLiteral("PlayerTopBar"));
    m_topBar->installEventFilter(this); // T46：顶层/页内顶栏空白区拖动窗口
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
    topLayout->setContentsMargins(24, 10, 24, 10);
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

    // 视频区域
    m_surfaceStack = new QStackedWidget(this);
    m_surfaceStack->setObjectName(QStringLiteral("VideoArea"));
    m_surfaceStack->installEventFilter(this); // T35：VideoArea 边缘双击/按压兜底
    m_placeholder = new VideoHostPlaceholder(m_surfaceStack);
    m_surfaceStack->addWidget(m_placeholder);
    root->addWidget(m_surfaceStack, 1);

    // 底部控制条：同样作为浮层，不占视频布局高度。
    auto *controls = new QVBoxLayout(m_controlBar);
    controls->setContentsMargins(24, 10, 24, 12);
    controls->setSpacing(8);

    auto *seekRow = new QHBoxLayout();
    seekRow->setSpacing(12);
    m_positionLabel = new QLabel(QStringLiteral("00:00"), m_controlBar);
    m_positionLabel->setObjectName(QStringLiteral("PlayerTimeLabel"));
    m_positionLabel->setMinimumWidth(48);
    m_durationLabel = new QLabel(QStringLiteral("00:00"), m_controlBar);
    m_durationLabel->setObjectName(QStringLiteral("PlayerTimeLabel"));
    m_durationLabel->setMinimumWidth(48);
    m_durationLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_seekSlider = new SeekSlider(m_controlBar);
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
    m_playButton->setFixedSize(48, 48);
    m_playButton->setToolTip(QStringLiteral("播放 / 暂停"));
    m_playButton->setCursor(Qt::PointingHandCursor);
    m_prevButton = makeIconButton(Icon::Prev, QStringLiteral("上一集"), QStringLiteral("PrevButton"),
                                  m_controlBar);
    m_nextButton = makeIconButton(Icon::Next, QStringLiteral("下一集"), QStringLiteral("NextButton"),
                                  m_controlBar);
    buttonRow->addStretch(1);
    buttonRow->addWidget(m_prevButton);
    buttonRow->addWidget(m_playButton);
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
    for (QPushButton *button : {m_subtitleButton, m_audioButton, m_statsButton}) {
        button->setFixedSize(36, 36);
        button->setIconSize(QSize(18, 18));
    }
    buttonRow->addWidget(m_subtitleButton);
    buttonRow->addWidget(m_audioButton);
    buttonRow->addSpacing(8);

    m_volumeIcon = new QLabel(m_controlBar);
    m_volumeIcon->setPixmap(iconPixmap(Icon::Volume, QColor(0x94, 0xA3, 0xB8), 20));
    m_volumeSlider = new QSlider(Qt::Horizontal, m_controlBar);
    m_volumeSlider->setObjectName(QStringLiteral("VolumeSlider"));
    m_volumeSlider->setRange(0, 100);
    m_volumeSlider->setValue(80);
    m_volumeSlider->setFixedWidth(96);
    m_volumeSlider->setCursor(Qt::PointingHandCursor);
    buttonRow->addWidget(m_volumeIcon);
    buttonRow->addWidget(m_volumeSlider);
    buttonRow->addSpacing(6);

    m_fullscreenButton = makeIconButton(Icon::Fullscreen, QStringLiteral("全屏"),
                                        QStringLiteral("FullscreenButton"), m_controlBar);
    buttonRow->addWidget(m_fullscreenButton);
    buttonRow->addWidget(m_statsButton);
    m_moreButton = makeIconButton(Icon::More, QStringLiteral("更多"), QStringLiteral("MoreButton"),
                                  m_controlBar);
    m_moreButton->hide();
    buttonRow->addWidget(m_moreButton);
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
    connect(m_moreButton, &QPushButton::clicked, this, [this]() {
        showCompactMenu(m_moreButton);
    });
    connect(m_statsButton, &QPushButton::clicked, this, [this]() {
        // 统计是 mpv OSD toggle，不改控制条状态；点击后重置自动隐藏并确保浮层在前。
        m_lastInteraction.restart();
        if (m_autoHide && m_playerModeActive)
            startAutoHideTimer();
        showOverlays();
        qInfo() << "PlayerPage: StatsButton clicked (toggle)";
        emit statsRequested();
    });
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
    if (auto *seek = static_cast<SeekSlider *>(m_seekSlider)) {
        seek->onSeek = [this](int value) {
            if (m_updatingSlider)
                return;
            m_lastInteraction.restart();
            if (m_autoHide && m_playerModeActive)
                startAutoHideTimer();
            m_positionLabel->setText(formatTimeValue(value));
            qInfo() << "PlayerPage: SeekSlider click value=" << value;
            emit seekRequested(value);
        };
    }

    // T51：控制条空白/按钮边缘/滑杆的按键日志与自动隐藏重置。
    m_controlBar->installEventFilter(this);
    for (QPushButton *button : m_controlBar->findChildren<QPushButton *>())
        button->installEventFilter(this);
    for (QPushButton *button : m_topBar->findChildren<QPushButton *>())
        button->installEventFilter(this);
    m_seekSlider->installEventFilter(this);
    m_volumeSlider->installEventFilter(this);
    for (QPushButton *button : m_controlBar->findChildren<QPushButton *>()) {
        connect(button, &QPushButton::clicked, this, [button]() {
            qInfo() << "PlayerPage: button clicked" << button->objectName();
        });
    }

    if (!m_topLevelOverlay)
        updateDockedOverlayGeometry();

    updateResponsiveControls();
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
    const int margin = kPlayerResizeHit;
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

bool PlayerPage::windowDragThresholdExceeded(const QPoint &pressGlobal,
                                                   const QPoint &currentGlobal)
{
    return (currentGlobal - pressGlobal).manhattanLength() > 4;
}

bool PlayerPage::handleWindowDragPress(const QPoint &globalPosition)
{
    if (!m_playerModeActive || m_fullscreen)
        return false;
    m_dragPressGlobal = globalPosition;
    m_dragPressed = true;
    m_dragStarted = false;
    return true;
}

bool PlayerPage::handleWindowDragMove(const QPoint &globalPosition, const char *source)
{
    if (!m_dragPressed || m_dragStarted || !m_playerModeActive || m_fullscreen)
        return false;
    if (!windowDragThresholdExceeded(m_dragPressGlobal, globalPosition))
        return false;

    m_dragStarted = true;
    QWindow *handle = window() ? window()->windowHandle() : nullptr;
    const bool started = handle && handle->startSystemMove();
    qInfo() << "PlayerPage::windowDrag source=" << source
            << "press" << m_dragPressGlobal << "current" << globalPosition
            << "distance" << (globalPosition - m_dragPressGlobal).manhattanLength()
            << "startSystemMove" << started;
    if (!started)
        qWarning() << "PlayerPage::windowDrag startSystemMove failed source=" << source;
    return true;
}

void PlayerPage::resetWindowDrag()
{
    m_dragPressed = false;
    m_dragStarted = false;
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

void PlayerPage::updateResponsiveControls()
{
    // T53：控制条默认/所有宽度均使用纯图标模式；文字只放在 tooltip。
    // T64：宽度 <520 或高度 <400 进入 compact：顶栏 44、控制条 64、次要按钮进「更多」。
    const bool compact = width() < 520 || height() < 400;
    m_compactLayout = compact;
    const auto setButtonText = [](QPushButton *button, const QString &fallback) {
        if (!button)
            return;
        QString fullText = button->property("fullText").toString();
        if (fullText.isEmpty()) {
            fullText = button->toolTip().isEmpty() ? fallback : button->toolTip();
            button->setProperty("fullText", fullText);
        }
        button->setText(QString());
        button->setToolTip(fullText);
        button->setAccessibleName(fullText);
    };
    setButtonText(m_subtitleButton, QStringLiteral("字幕"));
    setButtonText(m_audioButton, QStringLiteral("音轨"));
    setButtonText(m_statsButton, QStringLiteral("统计"));

    if (m_seekSlider)
        m_seekSlider->setFixedHeight(compact ? 18 : 28);
    if (m_playButton)
        m_playButton->setFixedSize(compact ? 36 : 48, compact ? 36 : 48);
    if (m_playButton)
        m_playButton->setIconSize(QSize(compact ? 20 : 24, compact ? 20 : 24));
    if (m_subtitleButton)
        m_subtitleButton->setVisible(!compact);
    if (m_audioButton)
        m_audioButton->setVisible(!compact);
    if (m_statsButton)
        m_statsButton->setVisible(!compact);
    if (m_fullscreenButton)
        m_fullscreenButton->setVisible(!compact);
    if (m_volumeIcon)
        m_volumeIcon->setVisible(!compact);
    if (m_volumeSlider) {
        m_volumeSlider->setVisible(!compact);
        m_volumeSlider->setFixedWidth(compact ? 72 : 96);
    }
    if (m_moreButton)
        m_moreButton->setVisible(compact);
    if (m_subtitleLabel)
        m_subtitleLabel->setVisible(!compact);
    if (auto *topLayout = qobject_cast<QHBoxLayout *>(m_topBar ? m_topBar->layout() : nullptr)) {
        topLayout->setContentsMargins(compact ? 12 : 24, compact ? 4 : 10,
                                     compact ? 12 : 24, compact ? 4 : 10);
        topLayout->setSpacing(compact ? 8 : 12);
    }
    if (auto *controls = qobject_cast<QVBoxLayout *>(m_controlBar ? m_controlBar->layout() : nullptr)) {
        controls->setContentsMargins(compact ? 8 : 24, compact ? 4 : 10,
                                     compact ? 8 : 24, compact ? 4 : 12);
        controls->setSpacing(compact ? 2 : 8);
    }
}

void PlayerPage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    updateResponsiveControls();
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
    // T51：控制条/按钮/滑杆的鼠标按下日志与自动隐藏重置。
    // 空白和按钮外扩区域统一在 PlayerControlBar 层分发，避免图标边缘/间隙丢点击。
    const bool isControlButton = qobject_cast<QPushButton *>(watched) != nullptr
                                 && qobject_cast<QWidget *>(watched)->parent() == m_controlBar;
    const bool isTopButton = qobject_cast<QPushButton *>(watched) != nullptr
                             && qobject_cast<QWidget *>(watched)->parent() == m_topBar;
    const bool isControlWidget = watched == m_controlBar || watched == m_seekSlider
                                 || watched == m_volumeSlider || isControlButton || isTopButton;
    if (isControlWidget && (event->type() == QEvent::MouseButtonPress
                            || event->type() == QEvent::MouseButtonRelease)) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            m_lastInteraction.restart();
            if (watched == m_seekSlider && event->type() == QEvent::MouseButtonPress)
                stopAutoHideTimer(); // 拖动进度条期间不自动隐藏
            else if (m_autoHide && m_playerModeActive)
                startAutoHideTimer();
            showOverlays();
            qInfo() << "PlayerPage: control mouse" << event->type()
                    << "watched=" << watched->objectName()
                    << "local=" << mouseEvent->position().toPoint()
                    << "global=" << mouseEvent->globalPosition().toPoint();
        }
        if (watched == m_controlBar && event->type() == QEvent::MouseButtonPress
            && mouseEvent->button() == Qt::LeftButton) {
            const QPoint pos = mouseEvent->position().toPoint();
            QPushButton *hit = nullptr;
            for (QPushButton *button : m_controlBar->findChildren<QPushButton *>()) {
                if (!button->isVisible() || !button->isEnabled())
                    continue;
                QRect rect(button->mapTo(m_controlBar, QPoint(0, 0)), button->size());
                rect.adjust(-6, -6, 6, 6);
                if (rect.contains(pos)) {
                    hit = button;
                    break;
                }
            }
            if (hit) {
                qInfo() << "PlayerPage: extended hit ->" << hit->objectName()
                        << "local=" << pos;
                hit->click();
                return true;
            }
        }
    }

    if (watched == m_surface || watched == m_surfaceStack || watched == m_topBar) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->button() == Qt::LeftButton) {
                if (watched != m_topBar
                    && tryStartEdgeResize(mouseEvent->globalPosition().toPoint())) {
                    mouseEvent->accept();
                    return true;
                }
                handleWindowDragPress(mouseEvent->globalPosition().toPoint());
            }
        } else if (event->type() == QEvent::MouseMove) {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            if (mouseEvent->buttons() & Qt::LeftButton)
                handleWindowDragMove(mouseEvent->globalPosition().toPoint(),
                                     watched == m_topBar ? "topBar" : "video");
        } else if (event->type() == QEvent::MouseButtonRelease
                   || event->type() == QEvent::MouseButtonDblClick) {
            resetWindowDrag();
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
    // T56：播放模式/全屏都不再留黑边；边缘缩放由 VideoHost/PlayerPage 的 6px 命中区负责。
    layout->setContentsMargins(0, 0, 0, 0);
    if (m_topLevelOverlay && m_playerModeActive)
        syncOverlayGeometry();
    else if (!m_topLevelOverlay)
        updateDockedOverlayGeometry();
}

void PlayerPage::refreshOverlayGeometry()
{
    applyVideoMargins();
    if (m_playerModeActive)
        syncOverlayGeometry();
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

    // T56：画面全铺满；T64 在 320×180 用 44px 顶栏 + 64px 控制条避免重叠。
    const bool compact = width() < 520 || height() < 400;
    const int topHeight = compact ? 44 : 76;
    const int maxTopWidth = qMax(200, host.width() - 24);
    const int topWidth = compact ? maxTopWidth
                                 : qMin(qMax(260, m_topBar->sizeHint().width()), maxTopWidth);
    m_topBar->setGeometry(host.left() + 12, host.top() + (compact ? 6 : 10), topWidth, topHeight);

    const int controlHeight = compact ? 64 : 108;
    m_controlBar->setGeometry(host.left(),
                              host.top() + host.height() - controlHeight,
                              qMax(0, host.width()), controlHeight);

    if (m_overlaysVisible) {
        m_topBar->raise();
        m_controlBar->raise();
    }
}

void PlayerPage::updateDockedOverlayGeometry()
{
    if (!m_topBar || !m_controlBar)
        return;
    const bool compact = width() < 520 || height() < 400;
    const int topHeight = compact ? 44 : qBound(58, height() / 9, 84);
    const int controlHeight = compact ? 64 : qBound(108, height() / 7, 124);
    m_topBar->setGeometry(0, 0, width(), topHeight);
    m_controlBar->setGeometry(0, height() - controlHeight, width(), controlHeight);
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
    m_topBar->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    m_controlBar->setAttribute(Qt::WA_TransparentForMouseEvents, false);
    m_topBar->show();
    m_controlBar->show();
    m_overlaysVisible = true;
    m_topBar->raise();
    m_controlBar->raise();
    qInfo() << "PlayerPage: overlays shown top=" << m_topBar->geometry()
            << "bottom=" << m_controlBar->geometry();
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
    if (auto *seek = dynamic_cast<SeekSlider *>(m_seekSlider)) {
        const int buffered = static_cast<int>(qMin<qint64>(
            m_durationMs, position + qMax<qint64>(1000, m_durationMs / 6)));
        seek->setBufferedValue(buffered);
    }
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
            const QString label = QStringLiteral("字幕：%1").arg(track.title);
            m_subtitleButton->setText(QString());
            m_subtitleButton->setToolTip(label);
            m_subtitleButton->setProperty("fullText", label);
            return;
        }
    }
    m_subtitleButton->setText(QString());
    m_subtitleButton->setToolTip(QStringLiteral("字幕"));
    m_subtitleButton->setProperty("fullText", QStringLiteral("字幕"));
}

void PlayerPage::setAudioTracks(const QList<TrackInfo> &tracks)
{
    m_audioTracks = tracks;
    for (const TrackInfo &track : tracks) {
        if (track.selected && !track.title.isEmpty()) {
            const QString label = QStringLiteral("音轨：%1").arg(track.title);
            m_audioButton->setText(QString());
            m_audioButton->setToolTip(label);
            m_audioButton->setProperty("fullText", label);
            return;
        }
    }
    m_audioButton->setText(QString());
    m_audioButton->setToolTip(QStringLiteral("音轨"));
    m_audioButton->setProperty("fullText", QStringLiteral("音轨"));
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

void PlayerPage::showCompactMenu(QPushButton *button)
{
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("CompactMoreMenu"));
    QAction *subtitle = menu.addAction(QStringLiteral("字幕"));
    QAction *audio = menu.addAction(QStringLiteral("音轨"));
    QAction *stats = menu.addAction(QStringLiteral("统计"));
    QAction *fullscreen = menu.addAction(QStringLiteral("全屏"));
    QAction *chosen = menu.exec(button->mapToGlobal(QPoint(0, -menu.sizeHint().height())));
    m_lastInteraction.restart();
    if (m_autoHide)
        startAutoHideTimer();
    showOverlays();
    if (chosen == subtitle)
        showTrackMenu(m_subtitleButton, m_subtitleTracks, false);
    else if (chosen == audio)
        showTrackMenu(m_audioButton, m_audioTracks, true);
    else if (chosen == stats)
        emit statsRequested();
    else if (chosen == fullscreen)
        emit fullscreenRequested();
}

void PlayerPage::showTrackMenu(QPushButton *button, const QList<TrackInfo> &tracks, bool audio)
{
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("TrackMenu"));
    const auto restoreOverlayAfterPopup = [this]() {
        if (!m_playerModeActive)
            return;
        m_lastInteraction.restart();
        if (m_autoHide)
            startAutoHideTimer();
        showOverlays();
        if (m_controlBar)
            m_controlBar->raise();
        if (m_topBar)
            m_topBar->raise();
        qInfo() << "PlayerPage: popup closed, overlay raised";
    };
    if (tracks.isEmpty()) {
        QAction *empty = menu.addAction(QStringLiteral("没有可用的轨道"));
        empty->setEnabled(false);
        menu.exec(button->mapToGlobal(QPoint(0, -menu.sizeHint().height())));
        restoreOverlayAfterPopup();
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
    restoreOverlayAfterPopup();
    if (!chosen || !chosen->data().isValid())
        return;
    const QString id = chosen->data().toString();
    if (audio)
        emit audioTrackSelected(id);
    else
        emit subtitleTrackSelected(id);
}

} // namespace ui