#include "playerpage.h"

#include "icons.h"

#include <QAction>
#include <QActionGroup>
#include <climits>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSlider>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace ui {
namespace {

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
        p.setRenderHint(QPainter::Antialiasing, true);

        QLinearGradient gradient(rect().topLeft(), rect().bottomRight());
        gradient.setColorAt(0.0, QColor(0x08, 0x0B, 0x12));
        gradient.setColorAt(0.55, QColor(0x0B, 0x11, 0x1C));
        gradient.setColorAt(1.0, QColor(0x05, 0x07, 0x0B));
        p.fillRect(rect(), gradient);

        const QRectF frame = QRectF(rect()).adjusted(22, 22, -22, -22);
        QPen pen(QColor(0x33, 0x41, 0x55, 150));
        pen.setStyle(Qt::DashLine);
        pen.setWidthF(1.2);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(frame, 12, 12);
    }
};

PlayerPage::PlayerPage(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("PlayerPage"));
    setAttribute(Qt::WA_StyledBackground, true);
    buildUi();
}

void PlayerPage::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // 顶部信息栏
    m_topBar = new QWidget(this);
    m_topBar->setObjectName(QStringLiteral("PlayerTopBar"));
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
    topLayout->addWidget(modeBadge);
    root->addWidget(m_topBar);

    // 视频区域
    m_surfaceStack = new QStackedWidget(this);
    m_surfaceStack->setObjectName(QStringLiteral("VideoArea"));
    m_placeholder = new VideoHostPlaceholder(m_surfaceStack);
    m_surfaceStack->addWidget(m_placeholder);
    root->addWidget(m_surfaceStack, 1);

    // 控制条
    m_controlBar = new QWidget(this);
    m_controlBar->setObjectName(QStringLiteral("PlayerControlBar"));
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
    root->addWidget(m_controlBar);

    connect(m_backButton, &QPushButton::clicked, this, &PlayerPage::backRequested);
    connect(m_playButton, &QPushButton::clicked, this, &PlayerPage::playPauseRequested);
    connect(m_prevButton, &QPushButton::clicked, this, &PlayerPage::previousEpisodeRequested);
    connect(m_nextButton, &QPushButton::clicked, this, &PlayerPage::nextEpisodeRequested);
    connect(m_subtitleButton, &QPushButton::clicked, this,
            [this]() { showTrackMenu(m_subtitleButton, m_subtitleTracks, false); });
    connect(m_audioButton, &QPushButton::clicked, this,
            [this]() { showTrackMenu(m_audioButton, m_audioTracks, true); });
    connect(m_statsButton, &QPushButton::clicked, this, &PlayerPage::statsRequested);
    connect(m_fullscreenButton, &QPushButton::clicked, this, &PlayerPage::fullscreenRequested);
    connect(m_volumeSlider, &QSlider::valueChanged, this, &PlayerPage::volumeChanged);
    connect(m_seekSlider, &QSlider::sliderMoved, this, [this](int value) {
        if (!m_updatingSlider)
            m_positionLabel->setText(formatTimeValue(value));
    });
    connect(m_seekSlider, &QSlider::sliderReleased, this, [this]() {
        if (!m_updatingSlider)
            emit seekRequested(m_seekSlider->value());
    });
}

void PlayerPage::attachVideoSurface(QWidget *surface)
{
    if (m_surface == surface)
        return;
    if (m_surface) {
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

void PlayerPage::setFullscreen(bool fullscreen)
{
    m_fullscreen = fullscreen;
    m_fullscreenButton->setIcon(icon(fullscreen ? Icon::ExitFullscreen : Icon::Fullscreen,
                                     QColor(0xE2, 0xE8, 0xF0), 22));
    m_fullscreenButton->setToolTip(fullscreen ? QStringLiteral("退出全屏") : QStringLiteral("全屏"));
}

void PlayerPage::setControlsVisible(bool visible)
{
    m_topBar->setVisible(visible);
    m_controlBar->setVisible(visible);
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